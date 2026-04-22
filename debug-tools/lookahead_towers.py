#!/usr/bin/env python3
"""
lookahead_towers.py — summarize all filament-lookahead towers in a gcode file.

Groups LOOKAHEAD_BLOCK_BEGIN/END spans by (base_layer, extruder) and reports
for each tower:
  - layer range covered (base..top)
  - extruder (filament) id
  - XY bounding box of all extrusion moves inside the tower's blocks
  - tool in the toolhead at tower start (most recent T<n> before the base block)
  - total extruded mm^3 in tower blocks
  - per-stack breakdown (lines, entity count, xy span)

Usage:
    python lookahead_towers.py <gcode_path> [--json]
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else __file__.rsplit("/", 1)[0])

from gcode_common import (
    load_lines, scan_tower_blocks, parse_g1, iter_axis_state,
    extract_tool_at_line, RE_TOOLCHANGE,
)


@dataclass
class TowerSummary:
    base_layer: int
    extruder: int
    top_layer: int = 0
    extra_layers: int = 0
    tool_at_start: int | None = None
    xmin: float = float("inf")
    xmax: float = float("-inf")
    ymin: float = float("inf")
    ymax: float = float("-inf")
    total_extrusion_mm: float = 0.0
    block_count: int = 0
    stacks: list[dict] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "base_layer": self.base_layer,
            "top_layer": self.top_layer,
            "extra_layers": self.extra_layers,
            "extruder": self.extruder,
            "tool_at_start": self.tool_at_start,
            "xy_bounds": {"xmin": round(self.xmin, 3), "ymin": round(self.ymin, 3),
                          "xmax": round(self.xmax, 3), "ymax": round(self.ymax, 3)}
                          if self.xmin != float("inf") else None,
            "total_extrusion_mm": round(self.total_extrusion_mm, 2),
            "block_count": self.block_count,
            "stacks": self.stacks,
        }


def summarize_towers(lines: list[str]) -> list[TowerSummary]:
    blocks = scan_tower_blocks(lines)
    # Group by (base_layer, extruder).
    by_tower: dict[tuple[int, int], list] = defaultdict(list)
    for b in blocks:
        by_tower[(b.base_layer, b.extruder)].append(b)

    summaries: list[TowerSummary] = []
    for (base, ext), blks in sorted(by_tower.items()):
        s = TowerSummary(base_layer=base, extruder=ext)
        # Find earliest block for tool_at_start detection.
        blks.sort(key=lambda b: b.begin_line)
        first = blks[0]
        s.tool_at_start = extract_tool_at_line(lines, first.begin_line - 1)
        s.extra_layers = first.extra_layers
        s.top_layer = base + s.extra_layers
        s.block_count = len(blks)

        # Walk each block's lines, track XY + E deltas.
        prev_e: float | None = None
        for b in blks:
            stack_xmin = stack_xmax = None
            stack_ymin = stack_ymax = None
            stack_ext_mm = 0.0
            stack_entity_hint = 0
            for i, state, upd in iter_axis_state(lines, b.begin_line + 1, b.end_line):
                x = state.get("X")
                y = state.get("Y")
                e = state.get("E")
                if "X" in upd or "Y" in upd:
                    if x is not None and y is not None:
                        if stack_xmin is None or x < stack_xmin: stack_xmin = x
                        if stack_xmax is None or x > stack_xmax: stack_xmax = x
                        if stack_ymin is None or y < stack_ymin: stack_ymin = y
                        if stack_ymax is None or y > stack_ymax: stack_ymax = y
                if "E" in upd and e is not None:
                    if prev_e is not None:
                        de = e - prev_e
                        if de > 0:
                            stack_ext_mm += de
                    prev_e = e
                # Rough entity-count heuristic: a new extrusion start after a
                # pure travel (E not in upd) hints at a new entity boundary.
                if "E" in upd and "X" in upd:
                    stack_entity_hint += 1
            if stack_xmin is not None:
                s.xmin = min(s.xmin, stack_xmin)
                s.xmax = max(s.xmax, stack_xmax)
                s.ymin = min(s.ymin, stack_ymin)
                s.ymax = max(s.ymax, stack_ymax)
            s.total_extrusion_mm += stack_ext_mm
            s.stacks.append({
                "layer":       b.layer,
                "stack_index": b.stack_index,
                "role":        b.role,
                "z":           b.z,
                "line_range":  [b.begin_line, b.end_line],
                "extrusion_mm": round(stack_ext_mm, 2),
                "xy_bounds":   {"xmin": round(stack_xmin, 3) if stack_xmin is not None else None,
                                "ymin": round(stack_ymin, 3) if stack_ymin is not None else None,
                                "xmax": round(stack_xmax, 3) if stack_xmax is not None else None,
                                "ymax": round(stack_ymax, 3) if stack_ymax is not None else None},
                "extrude_cmd_count": stack_entity_hint,
            })
        summaries.append(s)
    return summaries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode_path")
    ap.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    args = ap.parse_args()

    lines = load_lines(args.gcode_path)
    towers = summarize_towers(lines)

    if args.json:
        print(json.dumps([t.as_dict() for t in towers], indent=2))
        return

    print(f"Gcode file: {args.gcode_path}")
    print(f"Total lines: {len(lines)}")
    print(f"Tower count: {len(towers)}\n")
    for t in towers:
        print(f"Tower base_layer={t.base_layer} extruder={t.extruder}")
        print(f"  covers layers [{t.base_layer}..{t.top_layer}]  extra_layers={t.extra_layers}")
        print(f"  tool_in_head_at_start=T{t.tool_at_start}")
        if t.xmin != float("inf"):
            print(f"  xy_bounds: X[{t.xmin:.2f}..{t.xmax:.2f}]  Y[{t.ymin:.2f}..{t.ymax:.2f}]")
        else:
            print(f"  xy_bounds: <no XY extrusion found>")
        print(f"  total_extrusion_mm={t.total_extrusion_mm:.2f}  blocks={t.block_count}")
        print(f"  per-stack:")
        for s in t.stacks:
            xy = s["xy_bounds"]
            xystr = (f"X[{xy['xmin']:.2f}..{xy['xmax']:.2f}] Y[{xy['ymin']:.2f}..{xy['ymax']:.2f}]"
                     if xy["xmin"] is not None else "<no XY>")
            print(f"    layer={s['layer']:4d} stack={s['stack_index']:2d} "
                  f"role={s['role']:5s} z={s['z']} "
                  f"ext_mm={s['extrusion_mm']:7.2f} cmds={s['extrude_cmd_count']:4d}  {xystr}")
        print()


if __name__ == "__main__":
    main()
