"""The seam row-gain record (a0.seam-row-gain.v1) and its byte-stable writer.

Knots are placed in millimetres of the document (y = origin + (row + 0.5) *
pixel pitch), gains are dimensionless totals (the factor applied to B' at the
seam, global gain included) and slopes are per millimetre, so the record does
not depend on the raster it was estimated on.
"""
import hashlib
import math
import platform

import numpy as np

from . import __version__
from .rowfield import DESIGN_KEYS, REASON_BEYOND_LIMIT, REASON_NO_WINDOWS, REASON_TRIMMED

SCHEMA = "a0.seam-row-gain.v1"
MODEL = "seam-row-gain-v1"
STATE_APPLIED = "applied"
STATE_DECLINED = "declined"
REASON_TOO_MANY_UNUSED = "too-many-unused-knots"
INTERPOLATION = "log-linear"
FLOAT_DIGITS = 6


def environment():
    return {"python": platform.python_version(), "numpy": np.__version__, "a0seam": __version__}


def _knot_mm(row, req):
    return req.origin_y_mm + (row + 0.5) * req.pixel_pitch_um / 1000.0


def build_record(req, windows, knots, default_row_gain):
    """Assemble the record dict. default_row_gain: (height, 3) array or None."""
    design = req.design
    pitch_mm = req.pixel_pitch_um / 1000.0
    entries = []
    for k in knots:
        gain = k.gain
        if not k.used:
            gain = None if default_row_gain is None else default_row_gain[k.row]
        entries.append({
            "yMm": _knot_mm(k.row, req),
            "used": k.used,
            "reason": k.reason,
            "windows": k.windows,
            "gainBgr": None if gain is None else [float(v) for v in gain],
            "logSlopePerMm": None if k.log_slope_per_100px is None
            else [float(v) / (100.0 * pitch_mm) for v in k.log_slope_per_100px],
        })
    used = [k for k in knots if k.used]
    unused = len(knots) - len(used)
    declined = unused > design.max_unused_fraction * len(knots)
    by_reason = {r: sum(1 for k in knots if k.reason == r)
                 for r in (REASON_NO_WINDOWS, REASON_TRIMMED, REASON_BEYOND_LIMIT)}
    if used:
        g = np.array([k.gain for k in used])
        gain_range = {"min": [float(v) for v in g.min(axis=0)], "max": [float(v) for v in g.max(axis=0)]}
    else:
        gain_range = None
    return {
        "schema": SCHEMA,
        "model": MODEL,
        "state": STATE_DECLINED if declined else STATE_APPLIED,
        "reason": REASON_TOO_MANY_UNUSED if declined else None,
        "requestSha256": req.sha256,
        "environment": environment(),
        "raster": {
            "width": req.width, "height": req.height, "pixelPitchUm": req.pixel_pitch_um,
            "originYMm": req.origin_y_mm, "seamColumn": req.seam,
        },
        "globalGainBgr": [float(v) for v in req.global_gain],
        "design": {key: getattr(design, field) for key, field in DESIGN_KEYS.items()},
        "interpolation": INTERPOLATION,
        "windows": {"candidates": windows.candidates, "flat": int(windows.rows.size)},
        "knotSpacingMm": design.knot_spacing_rows * pitch_mm,
        "firstKnotMm": _knot_mm(knots[0].row, req),
        "knots": entries,
        "unusedKnots": unused,
        "unusedByReason": by_reason,
        "gainRangeBgr": gain_range,
    }


def _number(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, np.integer)):
        return str(int(v))
    f = float(v)
    if not math.isfinite(f):
        raise ValueError("record holds a non-finite number")
    s = f"{f:.{FLOAT_DIGITS}f}"
    return s[1:] if s.startswith("-") and float(s) == 0.0 else s


def _is_scalar_list(v):
    return isinstance(v, list) and all(not isinstance(i, (dict, list)) for i in v)


def _emit(v, indent, out):
    pad = "  " * indent
    if v is None:
        out.append("null")
    elif isinstance(v, str):
        out.append('"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"')
    elif isinstance(v, dict):
        if not v:
            out.append("{}")
            return
        out.append("{\n")
        for i, (key, val) in enumerate(v.items()):
            out.append(f'{pad}  "{key}": ')
            _emit(val, indent + 1, out)
            out.append(",\n" if i < len(v) - 1 else "\n")
        out.append(pad + "}")
    elif isinstance(v, list) and _is_scalar_list(v):
        out.append("[" + ", ".join(_inline(i) for i in v) + "]")
    elif isinstance(v, list):
        out.append("[\n")
        for i, val in enumerate(v):
            out.append(pad + "  ")
            _emit(val, indent + 1, out)
            out.append(",\n" if i < len(v) - 1 else "\n")
        out.append(pad + "]")
    else:
        out.append(_number(v))


def _inline(v):
    out = []
    _emit(v, 0, out)
    return "".join(out)


def dumps(record):
    """Serialize with fixed key order, two-space indent, 6 decimals and a final newline."""
    out = []
    _emit(record, 0, out)
    return "".join(out) + "\n"


def digest_without_environment(record):
    """SHA-256 of the serialized record without the environment block.

    The environment block names the Python and numpy versions, so it changes
    with the machine; the rest depends only on the request.
    """
    trimmed = {k: v for k, v in record.items() if k != "environment"}
    return hashlib.sha256(dumps(trimmed).encode("utf-8")).hexdigest()

