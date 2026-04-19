# Stretch Goals - Paint Tool Improvements

These are evaluated paint tool improvement ideas for future implementation, ranked by impact/effort ratio. The top 3 (Bounded Fill, Auto-Segment Face Groups, Sharp Edge Preview) are being implemented now and documented in GOALS.md.

## Precise Boundary Lines via Triangle Bisection

Upgrade boundary painter fill to split triangles along the boundary line for pixel-precise boundaries instead of the current "boundary triangles form a wall" approach. Research complete (2026-03-28): TriangleSelector's existing `split_triangle`/`perform_split` mechanism can be reused. The brush tool splits edges at midpoints based on edge length, creates child triangles with same `source_triangle`, and recursively tests cursor intersection. For boundary lines: (1) replace the edge-length test with a boundary-line-crossing test, (2) use the same `perform_split` to subdivide, (3) test which side of the boundary each child falls on. The fill would then paint only the children on the correct side. When filling from a triangle that has a boundary through it, split first and fill from the side the user clicked. Significant work but the mechanism is well-understood and proven. The current wall-of-triangles approach works well for most use cases.

## Refactor xyz Paint Tools into MeshAnalysis Module

Move the mesh analysis functions added by the xyz fork (precompute_vertex_curvature, get_sharp_edges, compute_face_groups) out of TriangleSelector and into a standalone `MeshAnalysis.hpp/cpp` utility. These are pure functions that don't depend on TriangleSelector state. The edge-snap brush and sharp edge preview would call MeshAnalysis instead of TriangleSelector methods. This reduces the invasiveness of the fork's changes to existing files and establishes the modular pattern used by the boundary painter.

## Lasso/Freehand Selection
Draw a freehand loop on the screen; all visible triangles whose projected centroids fall inside the loop are selected/painted. Most intuitive selection tool for arbitrary regions. Requires capturing mouse path as a 2D polygon, projecting triangle centroids to screen space, and point-in-polygon testing with backface culling. Moderate-high effort due to new 2D polygon capture and projection code needed.

## Select by Normal Direction
Select all triangles whose normals face within N degrees of a reference direction (e.g., "all upward-facing surfaces," "all surfaces facing the camera"). Nearly trivial to implement - face normals are already stored in `m_face_normals`, just needs a dot product filter and a UI angle slider. Could reuse the Smart Fill angle slider.

## Grow/Shrink Selection
Expand or contract the current painted selection by N triangle rings using adjacency. Useful for fine-tuning after Smart Fill or other selection tools. Simple BFS on existing `m_neighbors` adjacency data. Needs UI buttons/hotkeys and a ring count control.

## Isolation/Focus Mode
Temporarily hide all mesh regions except the currently selected/painted area, or hide a specific color to expose what's behind it. Helpful on complex models where interior faces are unreachable. The clipping plane partially addresses this. Moderate effort - needs rendering pipeline changes to skip certain triangles by state.

## Paint Depth Control
Allow configurable paint depth: "paint surface only," "paint through to next surface," "paint all depth," or a custom depth slider. Currently painting only affects top/bottom shell layers. Frequently requested feature (OrcaSlicer issues #5152, #6617, #12166).

## Color Replace Tool
Bulk-replace one painted color with another across the entire model. Like Photoshop's "Replace Color" but for triangle face assignments. Low effort - iterate all triangles in TriangleSelector and swap state values.
