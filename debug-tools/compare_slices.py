#!/usr/bin/env python3
"""
compare_slices.py — slice a lookahead-on 3MF and a lookahead-off baseline 3MF
through OrcaSlicer's CLI, then dissect both gcodes by physical Z and report
any per-layer object divergences. Used to verify that the Filament Lookahead
optimization (Phase 6 emission-time batching) preserves the model's actual
filament placement — i.e. the same filament prints in the same X/Y/Z
locations regardless of whether lookahead is on or off.

────────────────────────────────────────────────────────────────────────────
PURPOSE
────────────────────────────────────────────────────────────────────────────
The Filament Lookahead optimization rearranges WHEN tool changes happen:
isolated upper-layer regions of one filament get batched into a tower at
their first appearance instead of triggering a fresh tool change on every
layer. The XY footprint of every model voxel must remain unchanged — only
the gcode-emission ORDER differs. Manual inspection of two 12-MB gcode files
won't catch a subtle drift, so this tool does the comparison mechanically.

The bug class this catches:
  * lookahead places filament F at coordinates (X,Y,Z) but the baseline
    expected filament G there (or no extrusion at all)
  * lookahead's tower batch leaks into model regions outside the planner's
    isolated cluster
  * a tower's stack relocates content to the wrong physical Z
  * filament volumes drift away from baseline by more than path-discretization
    noise (e.g. flow scaling applied incorrectly during tower emission)
  * any feature gets dropped on the floor by the lookahead skip path

────────────────────────────────────────────────────────────────────────────
WHAT IT DOES (3 PASSES)
────────────────────────────────────────────────────────────────────────────
PASS 1 — slicing
  Kill any running orca-slicer; invoke the CLI on each 3MF; produce two
  separate gcode files (lookahead/plate_1.gcode and baseline/plate_1.gcode).
  Skippable with --reuse-gcode.

PASS 2 — gcode dissection (per file)
  Walk each gcode line-by-line tracking: physical Z (from G1 Z), current
  tool (from T<n>), current feature (from `; FEATURE:` comments), and a
  WIPE_TOWER bracket flag (from `; WIPE_TOWER_START/END`). Each extrusion
  move (G1 with positive E delta and XY motion) is recorded as a segment
  carrying its Z, tool, feature, and (start, end) XY.

  Segments are then classified:
    * WIPE TOWER       — anything bracketed by WIPE_TOWER_START/END
    * SUPPORT          — `; FEATURE: Support` or `; FEATURE: Support interface`
    * LOOKAHEAD_TOWER  — object-feature extrusion bracketed by
                         LOOKAHEAD_BLOCK_BEGIN/END (cluster content hoisted
                         into a tower batch — physical Z is correct because
                         the emitter raises Z for each upper stack)
    * OBJECT           — everything else (walls, infill, top/bottom surfaces,
                         bridges, gap fills, overhangs) outside an LA block

  OBJECT and LOOKAHEAD_TOWER segments are both aggregated into the object
  bucket — they represent the same model geometry, just emitted in different
  order. For each (Z bucket, filament tool):
    * total E length extruded (mm of filament — proportional to volume)
    * XY bounding box of all extrusion points

  Z bucketing rounds to 2 decimal places (0.01 mm), which is well below
  any realistic layer height and tolerates float jitter from absolute-mode
  parsing.

PASS 3 — comparison
  For every Z bucket present in either run, for every (Z, tool) pair,
  compare the OBJECT extrusion stats:

    * VOLUME_DIVERGENT — |la_e − bl_e| / max(la_e, bl_e) exceeds --tol-pct
    * BBOX_DIVERGENT   — any corner of the bbox differs by more than --tol-bbox-mm
    * MISSING_IN_LOOKAHEAD / EXTRA_IN_LOOKAHEAD — filament absent in one run

  Each violation becomes a flag entry with full numeric context.

────────────────────────────────────────────────────────────────────────────
OUTPUT
────────────────────────────────────────────────────────────────────────────
STDOUT (human-readable summary)
  * counts of Z buckets / filaments / segments per slice
  * total flag count and a one-line entry per flag
  * summary of which Z ranges and filaments are most affected

REPORT FILE (default: <out_dir>/comparison_report.json)
  Full machine-readable JSON containing:
    - inputs and tolerance settings
    - per-Z per-filament aggregates for both runs (volumes + bboxes)
    - the full flag list with all numeric context

  Pick this file up in followup tooling — it's flat enough for grep, jq,
  or a quick pandas DataFrame, and stable enough to diff between runs.

────────────────────────────────────────────────────────────────────────────
PRACTICAL USE
────────────────────────────────────────────────────────────────────────────
Run after every meaningful change to Filament Lookahead emission code
(GCode.cpp, FilamentLookahead.cpp, Print.cpp wipe-tower interaction,
WipeTower.cpp drainage, etc.) before committing. The expected result is
zero flags or only path-discretization noise (a small number of bbox
flags within ~0.3 mm). Anything beyond that is a regression.

Workflow:
  $ python debug-tools/compare_slices.py
  ... slices both 3MFs (~3 min) ...
  ... reports flags ...

Skip re-slicing during iterative tuning of tolerances:
  $ python debug-tools/compare_slices.py --reuse-gcode --tol-pct 0.5

Custom inputs (different test models):
  $ python debug-tools/compare_slices.py \\
      --lookahead-3mf "C:\\path\\to\\modelA.3mf" \\
      --baseline-3mf  "C:\\path\\to\\modelA_no_lookahead.3mf"

────────────────────────────────────────────────────────────────────────────
LIMITATIONS / CAVEATS
────────────────────────────────────────────────────────────────────────────
  * Both 3MFs must have the SAME wipe tower setting and SAME support config
    (e.g. "support: any PLA" + "interface: PVA" on both). Different settings
    will produce divergent support placement and noisy comparisons. The
    tool excludes wipe tower and support segments from the OBJECT compare,
    so minor wipe-tower differences are tolerated, but support placement
    differences will leak into adjacent object Z buckets if supports overlap.

  * "Volume" here means E-length (mm of 1.75 mm filament). To convert to
    mm³, multiply by π · (1.75/2)² ≈ 2.405. Comparisons are ratio-based, so
    the constant doesn't matter.

  * Path discretization noise ~0.5–2% volume / 0.1–0.3 mm bbox per layer
    is normal even between two slices of the same file (the slicer's
    rounding is not deterministic across runs in all paths). Default
    tolerances (1% / 2 mm) are picked above the typical noise floor.
    Tighten them only after observing the noise on your specific model.

  * The tool assumes OrcaSlicer-flavored gcode (FEATURE comments, etc.).
    Won't work on raw bambu_studio output unless feature markers are present.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gcode_common import load_lines, RE_TOOLCHANGE  # noqa: E402


# ─── defaults ─────────────────────────────────────────────────────────
DEFAULT_LOOKAHEAD_3MF = r"C:\Users\brian\Desktop\obj_1_tulipan_mod.3mf"
DEFAULT_BASELINE_3MF  = r"C:\Users\brian\Desktop\obj_1_tulipan_mod_no_look_ahead.3mf"
DEFAULT_SLICER_EXE    = r"C:\Users\brian\bin\orca_conf_gen\reference\orcaslicer\build\OrcaSlicer\orca-slicer.exe"
DEFAULT_OUT_DIR       = r"C:\Users\brian\AppData\Local\Temp\compare_slices"
DEFAULT_TOL_PCT       = 1.0    # E volume divergence threshold (percent)
DEFAULT_TOL_BBOX_MM   = 2.0    # bbox corner divergence threshold (mm)
SUPPORT_FEATURES = {"Support", "Support interface"}
# Whitelist of `; FEATURE:` values that count as model object extrusion.
# Anything else (Brim, Skirt, Custom, Prime tower, Unknown, ...) is excluded
# from the object-vs-object comparison, since those features don't represent
# the model's geometry per se and may legitimately differ between slices
# (e.g. brim path order, prime tower bands, custom gcode purge lines).
OBJECT_FEATURES = {
    "Outer wall", "Inner wall",
    "Top surface", "Bottom surface",
    "Sparse infill", "Internal solid infill", "Gap infill",
    "Internal Bridge", "Bridge", "Overhang wall",
}
# Maximum filament id we'll accept as a real tool change. T1000 etc. are
# slicer-internal sentinels that shouldn't be counted as filament tools.
MAX_FILAMENT_ID = 15


# ─── data structures ─────────────────────────────────────────────────
@dataclass
class Segment:
    z: float
    tool: int
    feature: str
    in_wipe_tower: bool
    in_lookahead_block: bool
    x_start: float
    y_start: float
    x_end: float
    y_end: float
    e_delta: float


@dataclass
class FilamentStat:
    e_total: float = 0.0
    xmin: float = float("inf")
    ymin: float = float("inf")
    xmax: float = float("-inf")
    ymax: float = float("-inf")
    seg_count: int = 0

    def add(self, x_start, y_start, x_end, y_end, e):
        self.e_total += e
        self.seg_count += 1
        for x in (x_start, x_end):
            if x < self.xmin: self.xmin = x
            if x > self.xmax: self.xmax = x
        for y in (y_start, y_end):
            if y < self.ymin: self.ymin = y
            if y > self.ymax: self.ymax = y

    def as_dict(self):
        return {
            "e_total": round(self.e_total, 4),
            "seg_count": self.seg_count,
            "bbox": [round(self.xmin, 3), round(self.ymin, 3),
                     round(self.xmax, 3), round(self.ymax, 3)],
        }


# ─── pass 1: slicing ──────────────────────────────────────────────────
def slice_file(input_3mf: str, output_dir: str, slicer_exe: str) -> str:
    """Slice a 3MF via Orca CLI, return the produced gcode path."""
    os.makedirs(output_dir, exist_ok=True)
    # Clear stale gcode (CLI keeps adding plate_1.gcode but we want fresh).
    for fname in os.listdir(output_dir):
        if fname.endswith(".gcode") or fname.endswith(".gcode.tmp"):
            try: os.remove(os.path.join(output_dir, fname))
            except OSError: pass

    if platform.system() == "Windows":
        ps_cmd = (
            f"Start-Process -FilePath '{slicer_exe}' "
            f"-ArgumentList '--slice 1 --outputdir \"{output_dir}\" \"{input_3mf}\"' "
            f"-WorkingDirectory '{os.path.dirname(slicer_exe)}' -Wait"
        )
        print(f"  slicing: {os.path.basename(input_3mf)} -> {output_dir}")
        result = subprocess.run(["powershell.exe", "-Command", ps_cmd],
                                capture_output=True, text=True)
    else:
        result = subprocess.run([slicer_exe, "--slice", "1",
                                 "--outputdir", output_dir, input_3mf],
                                capture_output=True, text=True)
    if result.returncode != 0:
        print(f"slicer exit {result.returncode}\nstderr:\n{result.stderr}",
              file=sys.stderr)
        sys.exit(1)

    candidates = [f for f in os.listdir(output_dir) if f.endswith(".gcode")]
    if not candidates:
        print(f"no .gcode in {output_dir} after slicing", file=sys.stderr)
        sys.exit(1)
    return os.path.join(output_dir, candidates[0])


# ─── pass 2: gcode dissection ─────────────────────────────────────────
RE_FEATURE = re.compile(r"^;\s*FEATURE:\s*(.+)$")
RE_X = re.compile(r"\sX([\-\d\.]+)")
RE_Y = re.compile(r"\sY([\-\d\.]+)")
RE_Z = re.compile(r"\sZ([\-\d\.]+)")
RE_E = re.compile(r"\sE([\-\d\.]+)")


def parse_segments(path: str) -> list[Segment]:
    """Walk the gcode once, emit one Segment per real extrusion move.

    Handles both absolute (M82) and relative (M83) extrusion modes. OrcaSlicer
    defaults to relative; in that mode each G1/G2/G3 line's E value IS the
    delta. In absolute mode the delta is new_e - prev_e (with G92 E<n>
    resetting prev_e). Also handles arc moves (G2/G3) which carry extrusion
    just like G1 in OrcaSlicer-flavored output.
    """
    lines = load_lines(path)
    cur_z = 0.0
    cur_tool: Optional[int] = None
    cur_feature = "Unknown"
    in_wipe_tower = False
    in_lookahead_block = False
    relative_e = True            # OrcaSlicer default; flipped by M82/M83
    prev_e_abs: float = 0.0      # only meaningful in absolute mode
    prev_x: Optional[float] = None
    prev_y: Optional[float] = None
    segments: list[Segment] = []

    for line in lines:
        s = line.strip()

        m = RE_TOOLCHANGE.match(s)
        if m:
            tid = int(m.group(1))
            if tid <= MAX_FILAMENT_ID:
                cur_tool = tid
            # else: T1000-style sentinel; ignore, leave cur_tool unchanged
            continue

        if s.startswith("; "):
            mf = RE_FEATURE.match(s)
            if mf:
                cur_feature = mf.group(1).strip()
                continue
            if "WIPE_TOWER_START" in s:
                in_wipe_tower = True
                continue
            if "WIPE_TOWER_END" in s:
                in_wipe_tower = False
                continue
            if "LOOKAHEAD_BLOCK_BEGIN" in s:
                in_lookahead_block = True
                continue
            if "LOOKAHEAD_BLOCK_END" in s:
                in_lookahead_block = False
                continue
            continue  # other comments are inert

        # E-mode switches (M82 = absolute, M83 = relative).
        if s.startswith("M82"):
            relative_e = False
            continue
        if s.startswith("M83"):
            relative_e = True
            continue

        # G92 sets the current "absolute" extruder position. In relative mode
        # it has no effect on deltas; in absolute mode it resets prev_e_abs.
        if s.startswith("G92"):
            ge = RE_E.search(" " + s)
            if ge and not relative_e:
                prev_e_abs = float(ge.group(1))
            continue

        # Extrusion-bearing motion: G0/G1 (linear) and G2/G3 (arc).
        if not (s.startswith("G1") or s.startswith("G0")
                or s.startswith("G2") or s.startswith("G3")):
            continue

        code = s.split(";", 1)[0]
        gx = RE_X.search(code)
        gy = RE_Y.search(code)
        gz = RE_Z.search(code)
        ge = RE_E.search(code)

        if gz:
            cur_z = float(gz.group(1))

        new_x = float(gx.group(1)) if gx else None
        new_y = float(gy.group(1)) if gy else None
        e_val = float(ge.group(1)) if ge else None

        # Compute the actual extrusion delta for this move.
        de: Optional[float] = None
        if e_val is not None:
            if relative_e:
                de = e_val   # relative: E on the line is the delta
            else:
                de = e_val - prev_e_abs
                prev_e_abs = e_val

        if (de is not None and de > 0.0
                and (new_x is not None or new_y is not None)
                and cur_tool is not None
                and prev_x is not None and prev_y is not None):
            ex = new_x if new_x is not None else prev_x
            ey = new_y if new_y is not None else prev_y
            segments.append(Segment(
                z=cur_z, tool=cur_tool, feature=cur_feature,
                in_wipe_tower=in_wipe_tower,
                in_lookahead_block=in_lookahead_block,
                x_start=prev_x, y_start=prev_y, x_end=ex, y_end=ey,
                e_delta=de,
            ))

        if new_x is not None:
            prev_x = new_x
        if new_y is not None:
            prev_y = new_y

    return segments


def classify(seg: Segment) -> str:
    # WIPE_TOWER takes priority — toolchange dance inside an LA block uses
    # the same WIPE_TOWER_START/END brackets, and that's tower fill (not
    # cluster geometry), so it should not be folded into object.
    if seg.in_wipe_tower:
        return "wipe_tower"
    if seg.feature in SUPPORT_FEATURES:
        # Support inside an LA block is still hoisted cluster geometry — its
        # physical Z matches baseline, so it counts as support either way.
        return "support"
    if seg.feature in OBJECT_FEATURES:
        # Object features inside an LA block are cluster content emitted at
        # the head's actual Z (the emitter raises Z for each upper stack), so
        # baseline puts that same content at that same Z. Tag separately so
        # the classification summary shows the split, but it folds into the
        # object aggregate for the comparison.
        return "lookahead_tower" if seg.in_lookahead_block else "object"
    return "other"  # brim, skirt, custom, unknown, prime tower, etc.


# Classes whose extrusion goes into the object-vs-object aggregate.
OBJECT_LIKE_CLASSES = {"object", "lookahead_tower"}


def aggregate_object(segments: list[Segment]) -> dict[float, dict[int, FilamentStat]]:
    """Group object + lookahead_tower segments by (Z bucket, filament).

    Both classes represent the model's geometry; lookahead just emits some
    of it inside a tower block at the same physical Z. They sum into one
    bucket so the comparison sees baseline's full layer content matched
    against (lookahead's normal-emit content + its tower-batched content).
    Z rounded to 0.01.
    """
    by_z: dict[float, dict[int, FilamentStat]] = {}
    for seg in segments:
        if classify(seg) not in OBJECT_LIKE_CLASSES:
            continue
        z_key = round(seg.z, 2)
        by_filament = by_z.setdefault(z_key, {})
        stat = by_filament.setdefault(seg.tool, FilamentStat())
        stat.add(seg.x_start, seg.y_start, seg.x_end, seg.y_end, seg.e_delta)
    return by_z


def classification_summary(segments: list[Segment]) -> dict:
    counts = {"wipe_tower": 0, "support": 0,
              "object": 0, "lookahead_tower": 0, "other": 0}
    e_totals = {"wipe_tower": 0.0, "support": 0.0,
                "object": 0.0, "lookahead_tower": 0.0, "other": 0.0}
    other_features: dict[str, int] = defaultdict(int)
    for seg in segments:
        cls = classify(seg)
        counts[cls] += 1
        e_totals[cls] += seg.e_delta
        if cls == "other":
            other_features[seg.feature] += 1
    return {"counts": counts,
            "e_totals": {k: round(v, 2) for k, v in e_totals.items()},
            "other_features_breakdown": dict(other_features)}


# ─── pass 3: comparison ───────────────────────────────────────────────
def compare(la: dict, bl: dict, tol_pct: float, tol_bbox: float) -> list[dict]:
    flags: list[dict] = []
    all_z = sorted(set(la.keys()) | set(bl.keys()))
    for z in all_z:
        la_f = la.get(z, {})
        bl_f = bl.get(z, {})
        all_tools = set(la_f.keys()) | set(bl_f.keys())
        for t in sorted(all_tools):
            la_d: Optional[FilamentStat] = la_f.get(t)
            bl_d: Optional[FilamentStat] = bl_f.get(t)

            if la_d is None and bl_d is not None:
                if bl_d.e_total > 0.01:  # ignore noise-level missing entries
                    flags.append({
                        "z": z, "tool": t, "reason": "MISSING_IN_LOOKAHEAD",
                        "baseline_e": round(bl_d.e_total, 4),
                        "baseline_bbox": [round(bl_d.xmin, 3), round(bl_d.ymin, 3),
                                          round(bl_d.xmax, 3), round(bl_d.ymax, 3)],
                    })
                continue
            if bl_d is None and la_d is not None:
                if la_d.e_total > 0.01:
                    flags.append({
                        "z": z, "tool": t, "reason": "EXTRA_IN_LOOKAHEAD",
                        "lookahead_e": round(la_d.e_total, 4),
                        "lookahead_bbox": [round(la_d.xmin, 3), round(la_d.ymin, 3),
                                           round(la_d.xmax, 3), round(la_d.ymax, 3)],
                    })
                continue

            la_e = la_d.e_total
            bl_e = bl_d.e_total
            denom = max(la_e, bl_e)
            if denom > 0.01:
                pct_diff = abs(la_e - bl_e) / denom * 100
                if pct_diff > tol_pct:
                    flags.append({
                        "z": z, "tool": t, "reason": "VOLUME_DIVERGENT",
                        "lookahead_e": round(la_e, 4),
                        "baseline_e":  round(bl_e, 4),
                        "pct_diff":    round(pct_diff, 3),
                    })

            la_box = [la_d.xmin, la_d.ymin, la_d.xmax, la_d.ymax]
            bl_box = [bl_d.xmin, bl_d.ymin, bl_d.xmax, bl_d.ymax]
            corner_diffs = [abs(la_box[i] - bl_box[i]) for i in range(4)]
            max_diff = max(corner_diffs)
            if max_diff > tol_bbox:
                flags.append({
                    "z": z, "tool": t, "reason": "BBOX_DIVERGENT",
                    "lookahead_bbox": [round(v, 3) for v in la_box],
                    "baseline_bbox":  [round(v, 3) for v in bl_box],
                    "max_corner_diff_mm": round(max_diff, 3),
                })
    return flags


# ─── orchestration ────────────────────────────────────────────────────
def stat_dict_to_json(by_z: dict[float, dict[int, FilamentStat]]) -> dict:
    return {
        f"{z:.2f}": {str(t): s.as_dict() for t, s in sorted(per_z.items())}
        for z, per_z in sorted(by_z.items())
    }


def main():
    ap = argparse.ArgumentParser(description="Filament Lookahead slice comparator")
    ap.add_argument("--lookahead-3mf", default=DEFAULT_LOOKAHEAD_3MF,
                    help="3MF with filament_lookahead enabled")
    ap.add_argument("--baseline-3mf", default=DEFAULT_BASELINE_3MF,
                    help="3MF with filament_lookahead disabled")
    ap.add_argument("--slicer-exe", default=DEFAULT_SLICER_EXE,
                    help="path to orca-slicer.exe")
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR,
                    help="output directory for sliced gcodes + report")
    ap.add_argument("--reuse-gcode", action="store_true",
                    help="skip slicing; use existing gcode files in out-dir")
    ap.add_argument("--tol-pct", type=float, default=DEFAULT_TOL_PCT,
                    help="object E-volume divergence threshold per-(Z,filament), %%")
    ap.add_argument("--tol-bbox-mm", type=float, default=DEFAULT_TOL_BBOX_MM,
                    help="object bbox corner divergence threshold, mm")
    ap.add_argument("--report", default=None,
                    help="JSON report path (default: <out-dir>/comparison_report.json)")
    ap.add_argument("--max-flags-stdout", type=int, default=50,
                    help="maximum flags to print to stdout (full set always in JSON)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    la_dir = os.path.join(args.out_dir, "lookahead")
    bl_dir = os.path.join(args.out_dir, "baseline")

    # Pass 1: slice (or reuse).
    if args.reuse_gcode:
        la_gcode = os.path.join(la_dir, "plate_1.gcode")
        bl_gcode = os.path.join(bl_dir, "plate_1.gcode")
        if not (os.path.exists(la_gcode) and os.path.exists(bl_gcode)):
            print("--reuse-gcode requested but expected gcodes not present", file=sys.stderr)
            print(f"  expected: {la_gcode}", file=sys.stderr)
            print(f"  expected: {bl_gcode}", file=sys.stderr)
            sys.exit(1)
        print(f"reusing existing gcodes:\n  {la_gcode}\n  {bl_gcode}")
    else:
        # kill any running slicer before invoking CLI on Windows
        if platform.system() == "Windows":
            subprocess.run(["powershell.exe", "-Command",
                            "Stop-Process -Name orca-slicer -Force -ErrorAction SilentlyContinue"],
                           capture_output=True)
        print("PASS 1: slicing both 3MFs through Orca CLI")
        la_gcode = slice_file(args.lookahead_3mf, la_dir, args.slicer_exe)
        bl_gcode = slice_file(args.baseline_3mf, bl_dir, args.slicer_exe)

    # Pass 2: parse + aggregate.
    print("PASS 2: parsing gcodes")
    la_segs = parse_segments(la_gcode)
    bl_segs = parse_segments(bl_gcode)
    la_cls = classification_summary(la_segs)
    bl_cls = classification_summary(bl_segs)
    def _summary(name, segs, c):
        return (f"  {name}: {len(segs):>7} segments  "
                f"obj={c['counts']['object']} la={c['counts']['lookahead_tower']} "
                f"sup={c['counts']['support']} "
                f"wt={c['counts']['wipe_tower']} other={c['counts']['other']}  "
                f"(E mm: obj={c['e_totals']['object']} la={c['e_totals']['lookahead_tower']} "
                f"sup={c['e_totals']['support']} "
                f"wt={c['e_totals']['wipe_tower']} other={c['e_totals']['other']})")
    print(_summary("lookahead", la_segs, la_cls))
    print(_summary("baseline ", bl_segs, bl_cls))

    la_obj = aggregate_object(la_segs)
    bl_obj = aggregate_object(bl_segs)
    all_z = sorted(set(la_obj.keys()) | set(bl_obj.keys()))
    print(f"  object Z buckets: lookahead={len(la_obj)} baseline={len(bl_obj)} union={len(all_z)}")

    # Pass 3: compare.
    print(f"PASS 3: comparing (tol_pct={args.tol_pct}%, tol_bbox={args.tol_bbox_mm}mm)")
    flags = compare(la_obj, bl_obj, args.tol_pct, args.tol_bbox_mm)
    by_reason: dict[str, int] = defaultdict(int)
    for f in flags:
        by_reason[f["reason"]] += 1
    flagged_z = sorted({f["z"] for f in flags})
    print(f"  flags: {len(flags)} (across {len(flagged_z)} Z bucket(s))")
    for r, n in sorted(by_reason.items()):
        print(f"    {r}: {n}")

    if flags:
        print(f"  first {min(args.max_flags_stdout, len(flags))} flags:")
        for f in flags[: args.max_flags_stdout]:
            line = f"    z={f['z']:>6}  T{f['tool']}  {f['reason']:<22} "
            if f["reason"] == "VOLUME_DIVERGENT":
                line += f"la={f['lookahead_e']}  bl={f['baseline_e']}  diff={f['pct_diff']}%"
            elif f["reason"] == "BBOX_DIVERGENT":
                line += f"max_corner_diff={f['max_corner_diff_mm']}mm  la={f['lookahead_bbox']}  bl={f['baseline_bbox']}"
            elif f["reason"] == "MISSING_IN_LOOKAHEAD":
                line += f"baseline_e={f['baseline_e']}  bbox={f['baseline_bbox']}"
            elif f["reason"] == "EXTRA_IN_LOOKAHEAD":
                line += f"lookahead_e={f['lookahead_e']}  bbox={f['lookahead_bbox']}"
            print(line)
        if len(flags) > args.max_flags_stdout:
            print(f"    ...{len(flags) - args.max_flags_stdout} more flags in JSON report")
    else:
        print("  no divergences within tolerances - slices match")

    # Write report.
    report_path = args.report or os.path.join(args.out_dir, "comparison_report.json")
    report = {
        "inputs": {
            "lookahead_3mf": args.lookahead_3mf,
            "baseline_3mf":  args.baseline_3mf,
            "lookahead_gcode": la_gcode,
            "baseline_gcode":  bl_gcode,
        },
        "tolerances": {
            "pct": args.tol_pct,
            "bbox_mm": args.tol_bbox_mm,
        },
        "classification": {
            "lookahead": la_cls,
            "baseline":  bl_cls,
        },
        "summary": {
            "object_z_buckets_lookahead": len(la_obj),
            "object_z_buckets_baseline":  len(bl_obj),
            "object_z_buckets_union":     len(all_z),
            "flag_count":                 len(flags),
            "flagged_z_count":            len(flagged_z),
            "flag_count_by_reason":       dict(by_reason),
        },
        "object_per_z": {
            "lookahead": stat_dict_to_json(la_obj),
            "baseline":  stat_dict_to_json(bl_obj),
        },
        "flags": flags,
    }
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"report: {report_path}")

    sys.exit(2 if flags else 0)


if __name__ == "__main__":
    main()
