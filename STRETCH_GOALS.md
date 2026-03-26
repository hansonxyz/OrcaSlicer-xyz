# Stretch Goals - Paint Tool Improvements

These are evaluated paint tool improvement ideas for future implementation, ranked by impact/effort ratio. The top 3 (Bounded Fill, Auto-Segment Face Groups, Sharp Edge Preview) are being implemented now and documented in GOALS.md.

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
