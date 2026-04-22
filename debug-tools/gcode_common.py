"""
Shared parsing helpers for gcode inspection tools (filament lookahead debug).

OrcaSlicer gcode conventions used:
- Per-layer blocks delimited by ``; CHANGE_LAYER`` followed by ``; Z_HEIGHT: X``
  and ``; LAYER_HEIGHT: Y`` comments.
- Tool changes emitted as ``T<n>`` on their own line (n is the 0-based
  filament id). The wipe tower's filament-change blocks also contain T<n>.
- Extrusion moves: ``G1 X.. Y.. E..`` (E positive = extrude, E negative =
  retract). Travel moves: ``G1 X.. Y..`` (no E).
- Absolute-mode Z moves: ``G1 Z<z>``.
- Lookahead markers (emitted by Phase 5c):
  ``; LOOKAHEAD_LAYER_INFO layer=L tower_filaments=<csv>``
  ``; LOOKAHEAD_BLOCK_BEGIN layer=L extruder=E base_layer=B stack_index=K
     extra_layers=N role=base|extra z=Z``
  ``; LOOKAHEAD_BLOCK_END layer=L extruder=E base_layer=B stack_index=K``
  ``; LOOKAHEAD_EXCLUSION_ZONE x_min=.. y_min=.. x_max=.. y_max=.. z_max=..``
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Iterator, Optional


# Regexes reused across tools.
RE_G1            = re.compile(r"^G[01]\b")
RE_AXIS          = re.compile(r"([XYZEF])([\-\d\.]+)")
RE_TOOLCHANGE    = re.compile(r"^T(\d+)\b")
RE_CHANGE_LAYER  = re.compile(r"^;\s*CHANGE_LAYER\b")
RE_Z_HEIGHT      = re.compile(r"^;\s*Z_HEIGHT:\s*([\-\d\.]+)")
RE_LAYER_HEIGHT  = re.compile(r"^;\s*LAYER_HEIGHT:\s*([\-\d\.]+)")
RE_LH_BLOCK_BEG  = re.compile(r"^;\s*LOOKAHEAD_BLOCK_BEGIN\s+(.*)")
RE_LH_BLOCK_END  = re.compile(r"^;\s*LOOKAHEAD_BLOCK_END\s+(.*)")
RE_LH_LAYER_INFO = re.compile(r"^;\s*LOOKAHEAD_LAYER_INFO\s+(.*)")
RE_LH_EXCL_ZONE  = re.compile(r"^;\s*LOOKAHEAD_EXCLUSION_ZONE\s+(.*)")


def parse_attrs(s: str) -> dict[str, str]:
    """Parse ``key=value`` tokens in a space-separated attribute string."""
    out = {}
    for tok in s.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


@dataclass
class LayerBlock:
    """A "layer block" in the gcode: from one CHANGE_LAYER to the next."""
    start_line: int            # index of the CHANGE_LAYER comment line
    end_line:   int            # exclusive — first line of next block (or EOF)
    z_height:   Optional[float] = None
    layer_height: Optional[float] = None
    # Layer index inferred from the first LOOKAHEAD_LAYER_INFO within the block.
    layer_idx:  Optional[int] = None


@dataclass
class TowerBlock:
    """A LOOKAHEAD_BLOCK_BEGIN/END span."""
    begin_line: int
    end_line:   int
    layer:      int
    extruder:   int
    base_layer: int
    stack_index: int
    extra_layers: int
    role:       str
    z:          Optional[float] = None


def load_lines(path: str) -> list[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        # Strip trailing newline + \r — makes downstream parsing cleaner.
        return [line.rstrip("\r\n") for line in f]


def scan_layers(lines: list[str]) -> list[LayerBlock]:
    """Find all per-layer blocks by CHANGE_LAYER boundaries."""
    blocks: list[LayerBlock] = []
    pending: Optional[LayerBlock] = None
    for i, line in enumerate(lines):
        if RE_CHANGE_LAYER.match(line):
            if pending is not None:
                pending.end_line = i
                blocks.append(pending)
            pending = LayerBlock(start_line=i, end_line=len(lines))
        elif pending is not None:
            m = RE_Z_HEIGHT.match(line)
            if m:
                pending.z_height = float(m.group(1))
                continue
            m = RE_LAYER_HEIGHT.match(line)
            if m:
                pending.layer_height = float(m.group(1))
                continue
            m = RE_LH_LAYER_INFO.match(line)
            if m:
                attrs = parse_attrs(m.group(1))
                if "layer" in attrs:
                    pending.layer_idx = int(attrs["layer"])
    if pending is not None:
        blocks.append(pending)
    return blocks


def scan_tower_blocks(lines: list[str]) -> list[TowerBlock]:
    """Parse LOOKAHEAD_BLOCK_BEGIN/END markers into TowerBlock spans."""
    stack: list[TowerBlock] = []
    finished: list[TowerBlock] = []
    for i, line in enumerate(lines):
        mb = RE_LH_BLOCK_BEG.match(line)
        if mb:
            attrs = parse_attrs(mb.group(1))
            tb = TowerBlock(
                begin_line=i, end_line=-1,
                layer=int(attrs.get("layer", "-1")),
                extruder=int(attrs.get("extruder", "-1")),
                base_layer=int(attrs.get("base_layer", "-1")),
                stack_index=int(attrs.get("stack_index", "-1")),
                extra_layers=int(attrs.get("extra_layers", "-1")),
                role=attrs.get("role", "?"),
                z=float(attrs["z"]) if "z" in attrs else None,
            )
            stack.append(tb)
            continue
        if RE_LH_BLOCK_END.match(line):
            if not stack:
                continue
            tb = stack.pop()
            tb.end_line = i
            finished.append(tb)
    return finished


def parse_g1(line: str) -> dict[str, float]:
    """Parse a G0/G1 line into axis -> value."""
    if not RE_G1.match(line):
        return {}
    # Strip any inline comment.
    code = line.split(";", 1)[0]
    return {m.group(1): float(m.group(2)) for m in RE_AXIS.finditer(code[3:])}


def iter_axis_state(lines: list[str], start: int, end: int) -> Iterator[tuple[int, dict[str, float], dict[str, float]]]:
    """Yield (line_idx, current_state, axis_updates) over lines[start:end].

    Tracks absolute X, Y, Z, E, F. Assumes absolute positioning.
    """
    state: dict[str, float] = {}
    for i in range(start, end):
        line = lines[i]
        upd = parse_g1(line)
        if not upd:
            continue
        state.update(upd)
        yield i, dict(state), upd


def extract_tool_at_line(lines: list[str], line_idx: int) -> Optional[int]:
    """Return the most recent T<n> command at or before line_idx."""
    for i in range(line_idx, -1, -1):
        m = RE_TOOLCHANGE.match(lines[i])
        if m:
            return int(m.group(1))
    return None
