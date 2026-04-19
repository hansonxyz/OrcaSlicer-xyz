# Archive: OpenBambuMonitorPanel v1 (rejected)

## Why archived
Built a completely new panel from scratch instead of making targeted edits to
the existing MonitorPanel. The result was a massive UI regression — lost all the
polished elements that already worked well (AMS visualization, temperature
displays, filament selection, camera integration, etc.).

## What was built
- OpenBambuMonitorPanel.hpp/cpp — custom wxPanel with:
  - Dark-themed sidebar (280px) with: status labels, temperature sparklines,
    storage file browser, printer discovery list
  - Content area with: camera placeholder, preview placeholder, control bar
  - MQTT status parsing via OpenBambuStatus model
  - Printer pairing dialog (access code + alias)
  - All controls wired (pause/resume/stop/speed/light/unload)

## What to salvage
- OpenBambuStatus.hpp/cpp — the data model and MQTT JSON parser are still useful.
  The PrinterStatus struct and parse_mqtt_status() can be used to feed data into
  the stock MonitorPanel's existing UI elements.
- TempHistory ring buffer — useful for sparkline feature when added to existing UI.
- DiscoveredPrinter struct — useful for the improved printer list.
- Printer pairing flow (access code + alias dialog) — can be added to existing UI.

## Correct approach
Make small, targeted edits to the existing MonitorPanel:
1. Remove unwanted sections (HMS, update assistant, home/XYZ controls)
2. Add new features (sparklines, printer list, storage browser) as additions
3. Keep everything that already works well (AMS, temps, filament, camera)
