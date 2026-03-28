#include "MeshPathFinder.hpp"
#include <queue>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

MeshPathFinder::MeshPathFinder(const std::vector<stl_vertex> &vertices,
                               const std::vector<Vec3i32>    &indices,
                               const std::vector<Vec3i32>    &neighbors)
    : m_vertices(vertices)
    , m_indices(indices)
    , m_neighbors(neighbors)
{
    build_vertex_adjacency();
}

void MeshPathFinder::build_vertex_adjacency()
{
    m_vertex_adj.resize(m_vertices.size());

    // For each triangle, register edges between its 3 vertices
    std::unordered_set<int64_t> seen_edges;
    for (size_t fi = 0; fi < m_indices.size(); ++fi) {
        const Vec3i32 &tri = m_indices[fi];
        for (int e = 0; e < 3; ++e) {
            int v0 = tri[e];
            int v1 = tri[(e + 1) % 3];
            // Canonical edge key
            int lo = std::min(v0, v1);
            int hi = std::max(v0, v1);
            int64_t key = (int64_t)lo << 32 | hi;
            if (seen_edges.count(key))
                continue;
            seen_edges.insert(key);

            float len = (m_vertices[v0] - m_vertices[v1]).norm();
            m_vertex_adj[v0].push_back({v1, len});
            m_vertex_adj[v1].push_back({v0, len});
        }
    }
}

std::vector<int> MeshPathFinder::find_path(int start_vertex, int end_vertex) const
{
    return find_path_weighted(start_vertex, end_vertex, nullptr);
}

std::vector<int> MeshPathFinder::find_path_weighted(
    int start_vertex, int end_vertex,
    const std::function<float(int, int)> &edge_weight) const
{
    if (start_vertex == end_vertex)
        return {start_vertex};
    if (start_vertex < 0 || end_vertex < 0 ||
        start_vertex >= (int)m_vertices.size() ||
        end_vertex >= (int)m_vertices.size())
        return {};

    const size_t n = m_vertices.size();
    const Vec3f &goal_pos = m_vertices[end_vertex];

    // A* with Euclidean heuristic
    struct Node {
        float f_score; // g + h
        int   vertex;
        bool operator>(const Node &o) const { return f_score > o.f_score; }
    };

    std::vector<float> g_score(n, std::numeric_limits<float>::max());
    std::vector<int>   came_from(n, -1);
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    g_score[start_vertex] = 0.f;
    float h = (m_vertices[start_vertex] - goal_pos).norm();
    open.push({h, start_vertex});

    while (!open.empty()) {
        auto [f, current] = open.top();
        open.pop();

        if (current == end_vertex) {
            // Reconstruct path
            std::vector<int> path;
            for (int v = end_vertex; v != -1; v = came_from[v])
                path.push_back(v);
            std::reverse(path.begin(), path.end());
            return path;
        }

        // Skip if we've already found a better route to this vertex
        float current_h = (m_vertices[current] - goal_pos).norm();
        if (f > g_score[current] + current_h + 1e-6f)
            continue;

        for (const auto &[neighbor, base_len] : m_vertex_adj[current]) {
            float weight = edge_weight ? edge_weight(current, neighbor) : 1.0f;
            float tentative_g = g_score[current] + base_len * weight;
            if (tentative_g < g_score[neighbor]) {
                g_score[neighbor] = tentative_g;
                came_from[neighbor] = current;
                float h_n = (m_vertices[neighbor] - goal_pos).norm();
                open.push({tentative_g + h_n, neighbor});
            }
        }
    }

    return {}; // no path found
}

int MeshPathFinder::nearest_vertex(const Vec3f &point, int facet_start, int max_rings) const
{
    if (facet_start < 0 || facet_start >= (int)m_indices.size())
        return -1;

    int best_vertex = -1;
    float best_dist_sq = std::numeric_limits<float>::max();

    std::vector<bool> visited(m_indices.size(), false);
    std::queue<int> q;
    q.push(facet_start);
    visited[facet_start] = true;

    int rings = 0, ring_size = 1;
    while (!q.empty() && rings < max_rings) {
        int next_ring_size = 0;
        for (int i = 0; i < ring_size && !q.empty(); ++i) {
            int cur = q.front();
            q.pop();
            const Vec3i32 &tri = m_indices[cur];
            for (int vi = 0; vi < 3; ++vi) {
                float dist_sq = (m_vertices[tri[vi]] - point).squaredNorm();
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best_vertex = tri[vi];
                }
            }
            for (int ni = 0; ni < 3; ++ni) {
                int neighbor = m_neighbors[cur](ni);
                if (neighbor >= 0 && !visited[neighbor]) {
                    visited[neighbor] = true;
                    q.push(neighbor);
                    ++next_ring_size;
                }
            }
        }
        ring_size = next_ring_size;
        ++rings;
    }

    return best_vertex;
}

int MeshPathFinder::nearest_vertex_on_triangle(const Vec3f &point, int facet_idx) const
{
    if (facet_idx < 0 || facet_idx >= (int)m_indices.size())
        return -1;

    const Vec3i32 &tri = m_indices[facet_idx];
    int best = tri[0];
    float best_dist = (m_vertices[tri[0]] - point).squaredNorm();
    for (int i = 1; i < 3; ++i) {
        float d = (m_vertices[tri[i]] - point).squaredNorm();
        if (d < best_dist) {
            best_dist = d;
            best = tri[i];
        }
    }
    return best;
}

std::vector<std::pair<int,int>> MeshPathFinder::path_to_edges(const std::vector<int> &path)
{
    std::vector<std::pair<int,int>> edges;
    edges.reserve(path.size());
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        int lo = std::min(path[i], path[i + 1]);
        int hi = std::max(path[i], path[i + 1]);
        edges.emplace_back(lo, hi);
    }
    return edges;
}

} // namespace Slic3r
