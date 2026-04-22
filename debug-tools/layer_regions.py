#!/usr/bin/env python3
"""
layer_regions.py — break a single gcode layer into extrusion "regions" and
report for each: which tool was active, whether it's inside a lookahead tower
block, extrusion volume (mm of E), and XY bounds.

A "region" here is a contiguous run of extrusion moves that share the same
active tool AND the same "lookahead mode" (either inside a particular
LOOKAHEAD_BLOCK or in normal-printing mode). Regions are separated by:
  - a tool change (T<n>)
  - entering or leaving a LOOKAHEAD_BLOCK span
  - (optionally) travel-only gaps — NOT split here to keep "region" aligned
    with tool/tower boundaries. Tight travel splits are future polish.

Also reports lookahead stuff stacked INTO this layer from a lower base layer
so you can see batched tower extras that physically print at this Z but were
emitted at an earlier gcode layer block.

Usage:
    python layer_regions.py <gcode_path> <layer_idx> [--json]
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else __file__.rsplit("/", 1)[0])

from gcode_common import (
    load_lines, scan_layers, scan_tower_blocks, parse_g1, iter_axis_state,
    RE_TOOLCHANGE, RE_LH_BLOCK_BEG, RE_LH_BLOCK_END, RE_LH_LAYER_INFO, parse_attrs,
)


@dataclass
class Region:
    tool: int | None
    mode: str   # "normal" or "tower"
    tower_base_layer: int | None = None
    tower_extruder:   int | None = None
    tower_stack_index: int | None = None
    tower_role: str | None = None
    line_start: int = 0
    line_end:   int = 0
    extrusion_mm: float = 0.0
    xmin: float = float("inf")
    xmax: float = float("-inf")
    ymin: float = float("inf")
    ymax: float = float("-inf")
    z_values: set = field(default_factory=set)

    def as_dict(self):
        return {
            "tool": self.tool,
            "mode": self.mode,
            "tower_base_layer": self.tower_base_layer,
            "tower_extruder": self.tower_extruder,
            "tower_stack_index": self.tower_stack_index,
            "tower_role": self.tower_role,
            "line_range": [self.line_start, self.line_end],
            "extrusion_mm": round(self.extrusion_mm, 2),
            "xy_bounds": {"xmin": round(self.xmin, 3), "ymin": round(self.ymin, 3),
                          "xmax": round(self.xmax, 3), "ymax": round(self.ymax, 3)}
                          if self.xmin != float("inf") else None,
            "z_values": sorted(round(z, 3) for z in self.z_values),
        }


def find_layer_block(layers, layer_idx: int):
    """Return the LayerBlock whose layer_idx matches (or None)."""
    for b in layers:
        if b.layer_idx == layer_idx:
            return b
    return None


def analyze_layer(lines, layer_idx: int, towers) -> dict:
    """Analyze a single layer_idx block.

    Returns a dict with:
      - layer_idx, z_height, layer_height
      - regions_in_block: regions emitted WITHIN this layer's gcode block
        (grouped by tool + tower mode).
      - extras_stacked_here: list of lookahead tower blocks whose z equals
        this layer's z_height but were emitted in a lower layer's gcode block
        (i.e. batched from below).
    """
    layers = scan_layers(lines)
    block = find_layer_block(layers, layer_idx)
    if not block:
        return {"error": f"no layer block with layer_idx={layer_idx}",
                "available_layer_indices": sorted({b.layer_idx for b in layers if b.layer_idx is not None})[:20] + ["..."]}

    # Find earliest tool state at block start.
    current_tool = None
    for i in range(block.start_line, -1, -1):
        m = RE_TOOLCHANGE.match(lines[i])
        if m:
            current_tool = int(m.group(1))
            break

    # Walk the block; build regions.
    tower_stack = []  # stack of active LOOKAHEAD_BLOCK records within this layer's gcode
    regions: list[Region] = []
    cur: Region | None = None
    prev_e: float | None = None

    def start_region(line_idx):
        nonlocal cur
        if cur is not None:
            cur.line_end = line_idx
            regions.append(cur)
        mode = "tower" if tower_stack else "normal"
        t = tower_stack[-1] if tower_stack else None
        cur = Region(tool=current_tool, mode=mode, line_start=line_idx, line_end=line_idx,
                     tower_base_layer=t.base_layer if t else None,
                     tower_extruder=t.extruder if t else None,
                     tower_stack_index=t.stack_index if t else None,
                     tower_role=t.role if t else None)

    start_region(block.start_line)

    for i in range(block.start_line, block.end_line):
        line = lines[i]
        m = RE_TOOLCHANGE.match(line)
        if m:
            current_tool = int(m.group(1))
            start_region(i)
            continue
        mb = RE_LH_BLOCK_BEG.match(line)
        if mb:
            attrs = parse_attrs(mb.group(1))
            class _T:
                pass
            t = _T()
            t.base_layer = int(attrs.get("base_layer", "-1"))
            t.extruder   = int(attrs.get("extruder", "-1"))
            t.stack_index = int(attrs.get("stack_index", "-1"))
            t.role = attrs.get("role", "?")
            t.z = float(attrs["z"]) if "z" in attrs else None
            tower_stack.append(t)
            start_region(i)
            continue
        me = RE_LH_BLOCK_END.match(line)
        if me:
            if tower_stack:
                tower_stack.pop()
            start_region(i)
            continue
        upd = parse_g1(line)
        if upd:
            cur.line_end = i
            state = {}
            # Build state by looking back — simplest: assume axes in upd are
            # absolute values (which they are in absolute mode).
            # But "X" only present in upd if it changed; so parse lines with
            # prior state tracking via iter_axis_state is cleaner. For one-pass
            # simplicity we just record whatever's in upd.
            if "X" in upd:
                cur.xmin = min(cur.xmin, upd["X"])
                cur.xmax = max(cur.xmax, upd["X"])
            if "Y" in upd:
                cur.ymin = min(cur.ymin, upd["Y"])
                cur.ymax = max(cur.ymax, upd["Y"])
            if "Z" in upd:
                cur.z_values.add(upd["Z"])
            if "E" in upd:
                e = upd["E"]
                if prev_e is not None:
                    de = e - prev_e
                    if de > 0:
                        cur.extrusion_mm += de
                prev_e = e

    if cur is not None:
        cur.line_end = block.end_line
        regions.append(cur)

    # Identify batched-in extras: any tower block whose z matches this layer's
    # z_height but begin_line is NOT within this layer's block range.
    extras_stacked_here = []
    if block.z_height is not None:
        for tb in towers:
            if tb.z is None:
                continue
            if abs(tb.z - block.z_height) < 0.01:
                if tb.begin_line < block.start_line or tb.begin_line >= block.end_line:
                    extras_stacked_here.append({
                        "base_layer": tb.base_layer,
                        "extruder":   tb.extruder,
                        "stack_index": tb.stack_index,
                        "role":       tb.role,
                        "z":          tb.z,
                        "emitted_in_gcode_of_layer_idx": None,  # filled below
                        "begin_line": tb.begin_line,
                        "end_line":   tb.end_line,
                    })
        # Find which layer_idx's gcode block each extra was emitted in.
        for ex in extras_stacked_here:
            for lb in layers:
                if lb.start_line <= ex["begin_line"] < lb.end_line:
                    ex["emitted_in_gcode_of_layer_idx"] = lb.layer_idx
                    break

    # Filter empty-regions and deduplicate tool+mode groupings for reporting.
    non_empty = [r for r in regions if r.extrusion_mm > 0 or r.line_end > r.line_start + 1]

    return {
        "layer_idx": layer_idx,
        "z_height":  block.z_height,
        "layer_height": block.layer_height,
        "gcode_line_range": [block.start_line, block.end_line],
        "regions_in_block": [r.as_dict() for r in non_empty],
        "extras_stacked_here_from_other_blocks": extras_stacked_here,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode_path")
    ap.add_argument("layer_idx", type=int)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    lines = load_lines(args.gcode_path)
    towers = scan_tower_blocks(lines)
    result = analyze_layer(lines, args.layer_idx, towers)

    if args.json:
        print(json.dumps(result, indent=2))
        return

    if "error" in result:
        print("ERROR:", result["error"])
        print("Available layer_idx sample:", result.get("available_layer_indices"))
        return

    print(f"Layer {result['layer_idx']}  z={result['z_height']}  layer_h={result['layer_height']}")
    print(f"Gcode line range: {result['gcode_line_range']}")
    print(f"\nRegions printed WITHIN this layer's gcode block:")
    if not result["regions_in_block"]:
        print("  (none)")
    grouped = {}
    for r in result["regions_in_block"]:
        key = (r["tool"], r["mode"], r["tower_base_layer"], r["tower_extruder"], r["tower_stack_index"])
        grouped.setdefault(key, []).append(r)
    for (tool, mode, tb, te, ts), rs in grouped.items():
        total_ext = sum(r["extrusion_mm"] for r in rs)
        xy_xmin = min(r["xy_bounds"]["xmin"] for r in rs if r["xy_bounds"])
        xy_xmax = max(r["xy_bounds"]["xmax"] for r in rs if r["xy_bounds"])
        xy_ymin = min(r["xy_bounds"]["ymin"] for r in rs if r["xy_bounds"])
        xy_ymax = max(r["xy_bounds"]["ymax"] for r in rs if r["xy_bounds"])
        all_z = sorted({z for r in rs for z in r["z_values"]})
        if mode == "tower":
            print(f"  TOOL=T{tool:<2d} mode=TOWER base_layer={tb} ext={te} stack={ts}")
        else:
            print(f"  TOOL=T{tool:<2d} mode=NORMAL")
        print(f"    regions={len(rs)}  total_ext_mm={total_ext:.2f}  "
              f"XY[{xy_xmin:.2f}..{xy_xmax:.2f}] x [{xy_ymin:.2f}..{xy_ymax:.2f}]")
        print(f"    z-values observed in moves: {all_z}")
    print(f"\nBatched-in extras (printed at this layer's Z but emitted in another layer's gcode block):")
    if not result["extras_stacked_here_from_other_blocks"]:
        print("  (none)")
    for ex in result["extras_stacked_here_from_other_blocks"]:
        print(f"  base_layer={ex['base_layer']} ext={ex['extruder']} "
              f"stack={ex['stack_index']} role={ex['role']} "
              f"emitted_in_layer_gcode={ex['emitted_in_gcode_of_layer_idx']}  "
              f"z={ex['z']}  lines={ex['begin_line']}..{ex['end_line']}")


if __name__ == "__main__":
    main()
