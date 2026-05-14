# OrcaSlicer-xyz

A community fork of [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer). The point of this fork is to experiment with quality-of-life features for multi-material printing and to replace the proprietary Bambu Lab networking DLL with an open-source LAN stack. It is not a polished product — features here are built, used by the maintainer, and shared in case they are useful to others.

**Version:** 2.3.2-xyz · **Base:** OrcaSlicer 2.3.2 · **Branch:** `xyz-main`

---

## What's different from upstream OrcaSlicer

### Filament Lookahead (multi-layer batched printing)

When a region uses a single filament for many consecutive layers, the slicer normally still does a tool change every layer because of how layers are ordered. Lookahead detects spatially isolated single-filament clusters and prints multiple layers of that filament back-to-back as a "tower stack" before switching. On four-color prints with tall single-color regions this can eliminate a large fraction of the tool changes and the associated wipe-tower purge volume.

It is implemented in three stages: an analysis pass on the per-layer extruder graph (`src/libslic3r/GCode/FilamentLookahead.cpp`) that plans candidate stacks under a set of containment / reachability rules, a gcode emission path in `src/libslic3r/GCode.cpp` that emits stacks as a batched block at the base layer, and a post-processor (`src/libslic3r/GCode/FilamentLookaheadPostProcessor.cpp`) that tags the output with marker comments. The gcode previewer parses those tags so the layer slider can expand sub-ticks for each tower stack — you can scroll through the emission order, not just the physical-Z order. The full spec lives in `FILAMENT_LOOKAHEAD.md`; viewer details are in `LOOKAHEAD_VIEWER.md`.

The feature is off by default. Enable via the **Filament Lookahead** process setting. Treat it as experimental — it has been driven against several test prints (see `debug-tools/` for the slice comparator) but edge cases around tree supports and complex multi-material interfaces are still being shaken out.

### OpenBambu — open-source LAN replacement for the Bambu Lab DLL

Stock OrcaSlicer talks to Bambu Lab printers through a closed-source DLL (`bambu_networking_*.dll`) loaded at runtime. OpenBambu is an in-tree replacement that speaks the same on-the-wire LAN protocols directly, using only OpenSSL:

- **SSDP** multicast discovery (with an extra NOTIFY listener so A1-series printers show up correctly), `OpenBambuDiscovery.cpp`
- **MQTT 3.1.1** over TLS:8883 for status reports and commands, `OpenBambuMqtt.cpp`
- **FTPS** (implicit TLS:990) for file upload, listing, download, delete, `OpenBambuFtp.cpp` and `OpenBambuFileSystem.cpp`
- Camera URL construction (RTSPS:322 for X1/P2S, custom `bambu:///` TCP+TLS:6000 for P1/A1)

Switch modes in **Preferences → Online → Connection**: *OpenBambu (LAN Only)* (default), *Bambu Lab Official (Stealth Mode)*, *Bambu Lab Official (Online Mode)*. The OpenBambu path requires no Bambu account and works on isolated networks. It also avoids a class of crashes seen in the proprietary DLL — see `BambuCrashGuard` below.

The MQTT layer distinguishes a CONNACK rc=4/5 (bad credentials) from a network failure: if the printer rejects the access code (common after a firmware update rotates it), the saved code is cleared and you get a "please re-enter the access code" prompt instead of an indefinite "Printer Offline" state. FTPS makes the same distinction for upload errors.

Code lives in `src/slic3r/Utils/OpenBambu/`.

### OpenBambu Device tab

When OpenBambu mode is active, a separate Device tab is used (`src/slic3r/GUI/OpenBambuMonitor/`). Differences from the stock Device tab:

- Sidebar printer list with quick-switch for users with multiple printers
- All AMS units shown in a single grid instead of tab-switching between them
- FTPS-backed media browser (timelapses, models, video) that doesn't need the BBL DLL or a cloud login
- Upload and HMS tabs removed (HMS codes are a cloud feature)
- Axis / extruder / bed manual-control widgets are hidden by default, since they tend to fight with the printer's own controls

### Color painting — boundary painter, bounded fill, edge snap

Additions to the MMU painting tools to make it less painful to paint clean color regions on complex models:

- **Boundary painter** (D key): click to drop points on the mesh surface; segments connect points via A* shortest-path along mesh edges. Hold Shift for planar "straight cut" mode. Close a polyline to make a fill barrier. Per-segment undo with Ctrl+Z. Marching-ants animation on completed boundaries. Boundaries persist when the project is saved as 3MF. Implementation: `src/libslic3r/MeshPathFinder.cpp` (standalone A*), `src/slic3r/GUI/Gizmos/PaintToolBoundary.cpp` (UI + 3MF persistence).
- **Unified Fill tool** (F key): three independent stop conditions — *Surface angle*, *Color boundaries*, *Boundary lines* — that can be combined. Disable all to flood the whole object.
- **Edge-snap sphere brush**: snaps the brush center to the nearest sharp mesh edge; threshold slider controls how sharp the edge must be.
- **Sharp-edge preview**: toggle to draw orange lines along edges that meet the angle threshold, so you can see where the fill / snap behavior will stop.

### "Any (Type)" support base filament

The **Support → Support/raft base** dropdown gains entries like *Any PLA*, *Any PETG*, etc. (one per non-soluble material type present in the project). Stock "Default" picks whichever filament is currently active; "Any (Type)" filters to filaments of that type and picks the cheapest one already in use on that layer. This matters for mixed-material projects where Default might otherwise pick a PETG filament to support a PLA region. Layer-by-layer resolution happens in `resolve_any_type_support_filament()` in `PrintConfig.cpp`. Encoded as a stable index into `MaterialType::all()` (`SUPPORT_FILAMENT_ANY_TYPE_BASE + idx`) so the saved value survives filament-count changes and printer-profile switches.

### Preserve filament colors / types on printer switch

Switching from, say, A1 0.4mm to X1C 0.6mm used to scramble the per-slot filament assignments — slot 1 might come back as a generic preset with the default white color. The fork remaps by slot index where possible, preserving slot count, colors, and types from the previous profile so visual identity stays consistent across printer changes. (`src/slic3r/GUI/Plater.cpp`, `src/libslic3r/PresetBundle.cpp`)

### Configurable "basic mode" settings list

The Process Settings panel has Basic / Advanced / Develop modes. Stock OrcaSlicer hardcodes which fields are in each. The fork reads an optional `basic_settings.cfg` from the OrcaSlicer data directory that lists which config keys should be visible in basic mode; groups with no visible fields hide themselves. If the file is absent the behavior is identical to stock. Useful for trimming the basic view to a personal "essentials" set without dropping into Advanced. Files: `src/slic3r/GUI/BasicSettingsConfig.cpp/hpp`, applied in `OG_CustomCtrl.cpp`.

### Prime tower early stop

If no filament changes remain above some layer, the prime tower for that layer is unnecessary. The fork drops the tower's `has_wipe_tower` flag from the topmost layer downward until the last layer that still requires a tool change. Smooth-timelapse mode disables this (the tower needs to be present every layer for consistent frame timing). In `ToolOrdering.cpp`'s wipe-tower partitioning.

### BambuCrashGuard

A Vectored Exception Handler installed when the proprietary Bambu DLL is loaded. It catches `0xC0000005` ACCESS_VIOLATION exceptions originating inside `bambu_networking_*.dll`, terminates the offending thread, and lets the rest of the app continue running instead of taking the whole process down. Only useful if you opt into the Bambu Lab plugin mode; OpenBambu mode does not load the DLL at all. `src/slic3r/Utils/BambuCrashGuard.cpp/hpp`. It can't catch stack-corruption crashes (`0xC0000409`) — those bypass user-mode handlers — but it does catch the more common null-deref pattern.

### Smaller things

- **Shift + middle-mouse rotate.** Plain middle-drag pans (stock behavior); Shift+middle drags rotate. Useful if you came from Blender / CAD tooling. (`GLCanvas3D::is_camera_rotate`)
- **`--dump-config` CLI flag.** Print the resolved process / printer / filament config to stdout as INI text. For debugging and for scripting workflows that need to inspect what the slicer will actually use.
- **Fork branding.** Version string `2.3.2-xyz`, fork-styled logo, About dialog points at this repo, auto-update URL points at this fork's GitHub releases instead of upstream.

---

## Building

Windows builds use Ninja + MSVC. See `CLAUDE.md` for full setup. Quick-start once dependencies are built:

```bash
powershell.exe -ExecutionPolicy Bypass -File xyz/build_artifacts/build_incremental.ps1
```

Output: `build/OrcaSlicer/orca-slicer.exe`. Linux and macOS use the upstream OrcaSlicer build flow — this fork has not been tested on either platform.

## Branches

- `xyz-main` — primary development branch, contains everything
- `main` — tracks upstream OrcaSlicer for rebases / merges
- Other branches are short-lived feature branches that get merged back into `xyz-main`

## Reporting issues

This is a hobby fork. There is no support channel, no roadmap, no SLA. Issues / discussions on the GitHub repo are welcome but may sit. Don't file bugs against upstream OrcaSlicer for behavior that only appears in this fork.

## Credits & License

Fork of **[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)** by SoftFever, itself based on [Bambu Studio](https://github.com/bambulab/BambuStudio) (Bambu Lab) and [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) (Prusa Research). Licensed under the [GNU AGPL v3](LICENSE.txt), same as upstream.
