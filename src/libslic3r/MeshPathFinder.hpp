#pragma once

#include "libslic3r.h"
#include <vector>
#include <functional>

namespace Slic3r {

// Standalone A* pathfinder on triangle mesh surfaces.
// Finds shortest vertex-to-vertex paths along mesh edges.
// No dependency on TriangleSelector or GUI code.
class MeshPathFinder {
public:
    // Initialize with mesh topology. Vectors are borrowed (not copied).
    // vertices:  position of each vertex (Vec3f)
    // indices:   3 vertex indices per triangle (Vec3i32)
    // neighbors: 3 neighbor triangle indices per triangle, -1 if none (Vec3i32)
    MeshPathFinder(const std::vector<stl_vertex> &vertices,
                   const std::vector<Vec3i32>    &indices,
                   const std::vector<Vec3i32>    &neighbors);

    // Find shortest path between two vertices. Returns ordered vertex indices
    // from start to end (inclusive). Empty if no path found.
    std::vector<int> find_path(int start_vertex, int end_vertex) const;

    // Find most direct path (topology distance — fewest edges, biased toward
    // the straight-line direction). Use when user holds Shift.
    std::vector<int> find_path_direct(int start_vertex, int end_vertex) const;

    // Find path by intersecting the mesh with a plane defined by the start/end
    // points and the averaged normals of their triangles. Returns vertex indices
    // (nearest vertex to each edge intersection) in the same format as find_path.
    // Use for shift-mode "straight line" boundaries.
    std::vector<int> find_path_planar(
        const Vec3f &start_pos, int start_facet,
        const Vec3f &end_pos, int end_facet,
        const Vec3f &start_normal, const Vec3f &end_normal) const;

    // Find shortest path with curvature-weighted edges.
    // edge_weight: optional per-edge cost multiplier. Called with (vertex_a, vertex_b).
    //   Return higher values to discourage crossing that edge (e.g., flat areas),
    //   lower values to attract the path (e.g., sharp edges).
    //   If null, uses Euclidean distance only.
    std::vector<int> find_path_weighted(
        int start_vertex, int end_vertex,
        const std::function<float(int, int)> &edge_weight) const;

    // Find the closest vertex to a 3D point (for converting hit positions to vertex IDs).
    // Searches within triangles reachable by BFS from facet_start within max_rings.
    int nearest_vertex(const Vec3f &point, int facet_start, int max_rings = 5) const;

    // Convert a triangle index + hit position to the nearest vertex on that triangle.
    int nearest_vertex_on_triangle(const Vec3f &point, int facet_idx) const;

    // Get edge set from a vertex path (consecutive pairs).
    // Returns edges as sorted (min,max) pairs for consistent hashing.
    static std::vector<std::pair<int,int>> path_to_edges(const std::vector<int> &path);

private:
    const std::vector<stl_vertex> &m_vertices;
    const std::vector<Vec3i32>    &m_indices;
    const std::vector<Vec3i32>    &m_neighbors;

    // Precomputed vertex adjacency: for each vertex, list of {neighbor_vertex, edge_length}
    struct VertexEdge {
        int   neighbor;
        float length;
    };
    std::vector<std::vector<VertexEdge>> m_vertex_adj;

    void build_vertex_adjacency();
};

} // namespace Slic3r
