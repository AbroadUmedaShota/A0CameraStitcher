"""The seam colour plan (a0.seam-color-plan.v1) built from an estimate.

The plan carries what the renderer needs and nothing else: the global gain G,
and for each knot used the total scale S at the seam (G included) and an offset.
Everything the estimator learned beyond that (how good each knot is, which
knots were left out and why, the constants used) goes into `diagnostics`, which
the reader does not interpret.

Knots are placed in millimetres of the document (see Request.row_center_mm),
scales are dimensionless factors on the 8-bit values of B (space "gamma8") and
slopes are per millimetre, so the plan does not depend on the raster it was
estimated on. How to read values between knots is the reader's rule, not part
of the plan: linear in yMm, end values held.
"""
import hashlib
import platform

import numpy as np

from . import __version__
from .jsonio import dumps
from .rowfield import DESIGN_KEYS, REASON_BEYOND_LIMIT, REASON_NO_WINDOWS, REASON_TRIMMED

SCHEMA = "a0.seam-color-plan.v1"
MODEL = "seam-row-gain-v1"
SOURCE_CAPTURE_ESTIMATE = "capture-estimate"
SPACE_GAMMA8 = "gamma8"
STATE_USABLE = "usable"
STATE_DECLINED = "declined"
REASON_TOO_MANY_UNUSED = "too-many-unused-knots"
REASON_TOO_FEW_KNOTS = "too-few-knots"
REASON_STRIP_RATIO = "strip-ratio-far-from-global"

# Binding kinds of a capture-estimate plan; all three are required.
BINDING_KINDS = ("condition-v3", "capture-original-a", "capture-original-b")
MIN_USED_KNOTS = 2
# The median of A / B' over the flat windows must stay within this factor of G
# (either way); outside it, B' was probably not the uncorrected sample. Proposal.
MAX_STRIP_RATIO_TO_GLOBAL = 1.08


def environment():
    return {"python": platform.python_version(), "numpy": np.__version__, "a0seam": __version__}


def strip_ratio_to_global(found, global_gain):
    """Median of A / B' over all flat windows, per channel, divided by G; None without windows."""
    if found.rows.size == 0:
        return None
    return np.median(found.mean_a / found.mean_b, axis=0) / np.asarray(global_gain, dtype=np.float64)


def _ratio_is_far(ratio):
    return bool((ratio > MAX_STRIP_RATIO_TO_GLOBAL).any() or (ratio < 1.0 / MAX_STRIP_RATIO_TO_GLOBAL).any())


def _floats(values):
    return [float(v) for v in values]


def _longest_gap_mm(knots, y_mm):
    """Longest stretch (mm) covered by a run of unused knots, from the used knot before it to the one after.

    A run at either end of the page extends to the first or last knot. 0 when every knot is used.
    """
    best, start = 0.0, None
    for i, k in enumerate(knots):
        if k.used:
            if start is not None:
                best = max(best, y_mm[i] - y_mm[max(start - 1, 0)])
                start = None
        elif start is None:
            start = i
    if start is not None:
        best = max(best, y_mm[-1] - y_mm[max(start - 1, 0)])
    return best


def _knot_diagnostics(knot, y_mm, pitch_mm):
    entry = {"yMm": y_mm, "used": knot.used, "reason": knot.reason, "windowsInRange": knot.windows_in_range,
             "windows": None, "effectiveCount": None, "xMeanMm": None, "xSpreadMm": None,
             "residualMadLogBgr": None, "logSlopePerMm": None, "seamScaleBgr": None}
    channels = knot.channels
    if channels is None:
        return entry
    entry["windows"] = [f.kept for f in channels]
    if not all(f.ok for f in channels):
        return entry
    mm_per_unit = 100.0 * pitch_mm     # x is kept in units of 100 px
    entry["effectiveCount"] = _floats(f.effective for f in channels)
    entry["xMeanMm"] = _floats(f.x_mean * mm_per_unit for f in channels)
    entry["xSpreadMm"] = _floats(f.x_spread * mm_per_unit for f in channels)
    entry["residualMadLogBgr"] = _floats(f.mad_log for f in channels)
    entry["logSlopePerMm"] = _floats(f.slope / mm_per_unit for f in channels)
    if knot.used:
        entry["seamScaleBgr"] = _floats(knot.scale)
    return entry


def build_plan(req, found, knots):
    """Assemble the plan dict. found: windows.FlatWindows; knots: rowfield.Knot list."""
    design = req.design
    y_mm = [req.row_center_mm(k.row) for k in knots]
    used = [k for k in knots if k.used]
    unused = len(knots) - len(used)
    ratio = strip_ratio_to_global(found, req.global_gain)
    if ratio is not None and _ratio_is_far(ratio):
        reason = REASON_STRIP_RATIO
    elif unused > design.max_unused_fraction * len(knots):
        reason = REASON_TOO_MANY_UNUSED
    elif len(used) < MIN_USED_KNOTS:
        reason = REASON_TOO_FEW_KNOTS
    else:
        reason = None
    by_reason = {r: sum(1 for k in knots if k.reason == r)
                 for r in (REASON_NO_WINDOWS, REASON_TRIMMED, REASON_BEYOND_LIMIT)}
    zero = [0.0, 0.0, 0.0]
    plan_knots = [] if reason else [
        {"yMm": y, "seamScaleBgr": _floats(k.scale), "seamOffsetBgr": zero}
        for k, y in zip(knots, y_mm) if k.used]
    return {
        "schema": SCHEMA,
        "source": SOURCE_CAPTURE_ESTIMATE,
        "state": STATE_DECLINED if reason else STATE_USABLE,
        "reason": reason,
        "space": SPACE_GAMMA8,
        "bindings": [{"kind": kind, "sha256": digest} for kind, digest in req.bindings],
        "requestSha256": req.sha256,
        "globalGainBgr": _floats(req.global_gain),
        "knots": plan_knots,
        "diagnostics": {
            "model": MODEL,
            "environment": environment(),
            "raster": {"widthPx": req.width, "heightPx": req.height, "dpi": req.dpi,
                       "topUm": req.top_um, "seamColumn": req.seam},
            "design": {key: getattr(design, field) for key, field in DESIGN_KEYS.items()},
            "windows": {"candidates": found.candidates, "flat": int(found.rows.size)},
            "stripRatioToGlobalBgr": None if ratio is None else _floats(ratio),
            "knotCount": len(knots),
            "usedKnots": len(used),
            "unusedKnots": unused,
            "unusedByReason": by_reason,
            "longestGapMm": _longest_gap_mm(knots, y_mm),
            "knots": [_knot_diagnostics(k, y, req.pitch_mm) for k, y in zip(knots, y_mm)],
        },
    }


def digest_without_environment(plan):
    """SHA-256 of the serialized plan without diagnostics.environment.

    The environment block names the Python and numpy versions, so it changes
    with the machine; the rest depends only on the request.
    """
    diagnostics = {k: v for k, v in plan["diagnostics"].items() if k != "environment"}
    trimmed = dict(plan, diagnostics=diagnostics)
    return hashlib.sha256(dumps(trimmed).encode("utf-8")).hexdigest()
