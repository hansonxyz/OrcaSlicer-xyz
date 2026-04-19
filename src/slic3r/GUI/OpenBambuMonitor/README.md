# OpenBambu Device Tab

Custom Device tab for OpenBambu mode, replacing the stock MonitorPanel when the
Bambu Labs integration is set to "OpenBambu (LAN Only)".

## Layout

```
+------------------------------------------------------------------+
|  [Sidebar ~280px]                  |  [Main Content Area]         |
|                                    |                              |
|  PRINTER STATUS                    |  +------------------------+  |
|  +--------------------------+      |  | Camera Stream           |  |
|  | Printer Name / Alias     |      |  | (resizable, reasonable  |  |
|  | Model: X1 Carbon         |      |  |  default ~40% height)  |  |
|  | Status: Printing 67%     |      |  +------------------------+  |
|  | Nozzle: 215°C / 220°C    |      |                              |
|  | Bed: 60°C / 60°C         |      |  +------------------------+  |
|  | Layer: 142 / 312         |      |  | Live Print Preview      |  |
|  | Time left: 1h 23m        |      |  | (GCode viewer showing   |  |
|  | Speed: Standard (100%)   |      |  |  progress by layer)     |  |
|  | Fan: 80%                 |      |  | [PLACEHOLDER Phase 2]   |  |
|  +--------------------------+      |  +------------------------+  |
|                                    |                              |
|  STORAGE                           |  +------------------------+  |
|  +--------------------------+      |  | Controls                |  |
|  | SD Card: 12.3 GB free    |      |  | [Pause] [Resume] [Stop] |  |
|  | [Show in Explorer] (*)   |      |  | Speed: [v Standard   ]  |  |
|  | [Browse Files]           |      |  | Light: [On] [Off]       |  |
|  | file1.3mf          [>]  |      |  | Extruder: [Unload]      |  |
|  | file2.3mf          [>]  |      |  +------------------------+  |
|  | file3.3mf          [>]  |      |                              |
|  +--------------------------+      |                              |
|                                    |                              |
|  PRINTERS                          |                              |
|  +--------------------------+      |                              |
|  | > X1 Carbon "Brian's"  * |     |                              |
|  |   A1 Mini "Workshop"     |      |                              |
|  |   P1S (not connected)    |      |                              |
|  +--------------------------+      |                              |
+------------------------------------------------------------------+
```

## Phase 1 (Current Implementation)

### Sidebar

**Printer Status** — live from MQTT:
- Printer alias (user-assigned) and model name
- Print state (idle/printing/paused) with progress %
- Nozzle temp (current / target) with rolling 5-min sparkline
- Bed temp (current / target) with rolling 5-min sparkline
- Current layer / total layers
- Time remaining estimate
- Speed preset and percentage
- Part cooling fan speed
- AMS tray visualization (colors + filament types, from existing UI pattern)

**Storage** — via FTPS directory listing:
- SD card file list (NLST from FTP root)
- File count / free space (if available)
- Click file → context menu: Preview GCode [placeholder], Print from SD
- "Show in Explorer" button [placeholder — future WebDAV proxy]
- "Browse Files" expands the file list

**Printers** — discovered via SSDP, persisted aliases:
- Live list of all discovered printers on LAN
- Each entry shows: alias (or default name), model, signal, status indicator
- Click to connect (MQTT auto-connects)
- First-time click on unknown printer → modal for access code + alias
- Auto-selects first live printer on tab open (unless a print was sent this session)
- Aliases + access codes stored in AppConfig keyed by serial number

### Main Content Area

**Camera** — reuses MediaPlayCtrl:
- Reasonable default size (~40% of content area height, not fullscreen)
- Resizable via splitter/sash

**Live Print Preview** [Phase 2 PLACEHOLDER]:
- Area reserved for GCode layer viewer
- Will fetch current print file from printer via FTPS
- Show layer-by-layer progress matching current `layer_num`
- For now: shows "Live preview — coming in a future update" placeholder

**Controls**:
- Pause / Resume / Stop buttons
- Speed preset dropdown
- Chamber light on/off
- Extruder unload button
- NO home/X/Y/Z/bed up/bed down controls (useless)

## Phase 2 (Future Goals)

### Live GCode Print Preview
- Fetch the currently printing .3mf/.gcode from the printer via FTPS
- Parse and render in a GCode viewer (reuse existing GCodeViewer component)
- Highlight current layer based on MQTT `layer_num`
- Animated progress showing completed vs remaining layers

### Preview GCode from Storage
- Select a file in Storage → "Preview GCode" button
- Downloads the file via FTPS to a temp directory
- Opens in the GCode viewer
- From the viewer: "Print this file" button sends `project_file` MQTT command
  (file already on printer, no re-upload needed)

### Show in Explorer (Windows)
- Starts a local WebDAV-to-FTPS proxy server
- Maps the printer's SD card as a WebDAV mount
- Opens Windows Explorer to the mounted path
- User can browse, drag-drop, delete files naturally
- Windows-only initially (macOS/Linux: native FUSE mount possible later)

### Other Ideas
- Print history (parse the prints/ directory on SD card)
- Timelapse management (browse/download from timelapse/ directory)
- Estimated filament usage for current print
- Multi-printer dashboard (split view showing status of all connected printers)

## Architecture

- `OpenBambuMonitorPanel.hpp/cpp` — main panel, owns the layout
- `OpenBambuStatusPanel.hpp/cpp` — sidebar status display (MQTT-driven)
- `OpenBambuPrinterList.hpp/cpp` — sidebar printer discovery list
- `OpenBambuStoragePanel.hpp/cpp` — sidebar file browser (FTPS-driven)
- Reuses `MediaPlayCtrl` for camera
- Reuses `OpenBambu::Agent` for all protocol operations
- Printer aliases stored in AppConfig under `openbambu_printer_aliases`
- Access codes already stored in `user_access_code` (existing OrcaSlicer mechanism)

## Files Removed from Stock MonitorPanel
- HMS/health monitoring panel (useless noise)
- Firmware update assistant (handled by printer touchscreen)
- Home/X/Y/Z/bed movement controls (useless for normal operation)
- Collapsed printer dropdown (replaced by always-visible list)
