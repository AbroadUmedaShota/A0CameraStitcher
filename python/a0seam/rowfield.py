"""Per-knot fit of the seam gain field.

For every knot row the gain B' needs to match A at the seam is estimated from
the flat windows around it: per colour channel, log(A / B') is fitted as
r0 + r1 * (x - seam) / 100 px with triangular row weights and one trimming
pass. exp(r0) is the gain at the seam; the slope r1 is only recorded.

A knot is unused (the renderer keeps its default there) when it has too few
windows, when trimming leaves too few, or when its gain is further from the
global gain than the limit. Nothing is filled in from neighbouring knots.
"""
from dataclasses import dataclass, fields

import numpy as np

# --- Design constants -------------------------------------------------------
# One place for every tunable of the estimator. These are proposals from the
# design study, not approved values; the record carries the values actually
# used, and a request may override them (see request.py).
STEP_PX = 8                  # lattice step of window centres
MEAN_RADIUS_PX = 10          # half width of the colour mean window (21 x 21)
FLAT_RADIUS_PX = 22          # half width of the flatness window (45 x 45)
FLAT_STD_DN = 3.0            # luminance standard deviation limit inside the flatness window
SEAM_HALF_WIDTH_PX = 96      # windows are taken at most this far from the seam column
ROW_RADIUS_ROWS = 128        # half width of the triangular row weights
KNOT_SPACING_ROWS = 32       # knot spacing (180 dpi: 4.52 mm)
MIN_WINDOWS = 8              # windows (and windows left after trimming) a knot needs
TRIM_MADS = 3.0              # residuals beyond this many scaled MADs are dropped once
GAIN_LIMIT = 1.25            # allowed factor between a knot gain and the global gain
MAX_UNUSED_FRACTION = 0.25   # more unused knots than this: the estimate is declined
MIN_LEVEL_DN = 1.0           # guard (not in the design): a window mean below this cannot give a ratio
MAX_LEVEL_DN = 254.0         # guard (not in the design): a window mean above this is clipped
# -----------------------------------------------------------------------------

_MAD_SCALE = 1.4826
_MAD_FLOOR = 0.002           # in log units; keeps the trim from collapsing on noise-free data


@dataclass(frozen=True)
class Design:
    step_px: int = STEP_PX
    mean_radius_px: int = MEAN_RADIUS_PX
    flat_radius_px: int = FLAT_RADIUS_PX
    flat_std_dn: float = FLAT_STD_DN
    seam_half_width_px: int = SEAM_HALF_WIDTH_PX
    row_radius_rows: int = ROW_RADIUS_ROWS
    knot_spacing_rows: int = KNOT_SPACING_ROWS
    min_windows: int = MIN_WINDOWS
    trim_mads: float = TRIM_MADS
    gain_limit: float = GAIN_LIMIT
    max_unused_fraction: float = MAX_UNUSED_FRACTION
    min_level_dn: float = MIN_LEVEL_DN
    max_level_dn: float = MAX_LEVEL_DN

    @property
    def reach(self):
        """How far from the seam column the strip must extend on each side."""
        return (self.seam_half_width_px // self.step_px) * self.step_px + self.flat_radius_px

    def problems(self):
        """Reasons this design cannot be used; empty when it is acceptable."""
        out = []

        def need(cond, text):
            if not cond:
                out.append(text)
        need(1 <= self.step_px <= 1024, "stepPx must be 1..1024")
        need(1 <= self.mean_radius_px <= self.flat_radius_px <= 1024,
             "radii must satisfy 1 <= meanRadiusPx <= flatRadiusPx <= 1024")
        need(0 <= self.seam_half_width_px <= 4096, "seamHalfWidthPx must be 0..4096")
        need(1 <= self.row_radius_rows <= 8192, "rowRadiusRows must be 1..8192")
        need(1 <= self.knot_spacing_rows <= 8192, "knotSpacingRows must be 1..8192")
        need(3 <= self.min_windows <= 1000000, "minWindows must be at least 3")
        need(0.0 <= self.flat_std_dn <= 255.0, "flatStdDn must be 0..255")
        need(0.0 < self.trim_mads <= 100.0, "trimMads must be positive")
        need(1.0 < self.gain_limit <= 100.0, "gainLimit must be above 1")
        need(0.0 <= self.max_unused_fraction <= 1.0, "maxUnusedFraction must be 0..1")
        need(0.0 <= self.min_level_dn < self.max_level_dn <= 255.0,
             "level guards must satisfy 0 <= minLevelDn < maxLevelDn <= 255")
        return out


DEFAULT_DESIGN = Design()

# JSON name -> Design field, in the order the record writes them.
DESIGN_KEYS = {
    "stepPx": "step_px",
    "meanRadiusPx": "mean_radius_px",
    "flatRadiusPx": "flat_radius_px",
    "flatStdDn": "flat_std_dn",
    "seamHalfWidthPx": "seam_half_width_px",
    "rowRadiusRows": "row_radius_rows",
    "knotSpacingRows": "knot_spacing_rows",
    "minWindows": "min_windows",
    "trimMads": "trim_mads",
    "gainLimit": "gain_limit",
    "maxUnusedFraction": "max_unused_fraction",
    "minLevelDn": "min_level_dn",
    "maxLevelDn": "max_level_dn",
}
assert set(DESIGN_KEYS.values()) == {f.name for f in fields(Design)}

REASON_NO_WINDOWS = "no-windows"
REASON_TRIMMED = "too-few-after-trim"
REASON_BEYOND_LIMIT = "beyond-gain-limit"


@dataclass(frozen=True)
class Knot:
    row: int
    windows: int
    used: bool
    reason: object               # None when used
    gain: object                 # (3,) B G R total gain at the seam, exp(r0); None when no fit
    log_slope_per_100px: object  # (3,) or None


def knot_rows(height, design):
    """Rows of the knots: 0, spacing, 2 * spacing ... and always the last row."""
    rows = list(range(0, height - 1, design.knot_spacing_rows))
    rows.append(height - 1)
    return rows


def _wls_line(w, x, y):
    """Weighted least squares of y = a0 + a1 * x. A degenerate x spread gives a flat line."""
    sw = w.sum()
    sx, sy = (w * x).sum(), (w * y).sum()
    sxx, sxy = (w * x * x).sum(), (w * x * y).sum()
    den = sw * sxx - sx * sx
    if den > 1e-9 * sw * sw:
        a1 = (sw * sxy - sx * sy) / den
        return (sy - a1 * sx) / sw, a1
    return sy / sw, 0.0


def _fit_channel(w0, x, y, design):
    """One trimming pass. Returns (a0, a1) or None when too few samples remain."""
    a0, a1 = _wls_line(w0, x, y)
    e = y - a0 - a1 * x
    mad = np.median(np.abs(e)) * _MAD_SCALE + _MAD_FLOOR
    keep = np.abs(e) < design.trim_mads * mad
    if int(keep.sum()) < design.min_windows:
        return None
    return _wls_line(w0 * keep, x, y)


def fit_knots(windows, seam, global_gain, height, design):
    """Fit every knot. windows: windows.FlatWindows; global_gain: (3,) B G R."""
    order = np.argsort(windows.rows, kind="stable")
    rows = windows.rows[order]
    x = (windows.columns[order] - seam) / 100.0
    log_ratio = np.log(windows.mean_a[order] / windows.mean_b[order])
    log_global = np.log(np.asarray(global_gain, dtype=np.float64))
    log_limit = np.log(design.gain_limit)
    r = design.row_radius_rows

    knots = []
    for yk in knot_rows(height, design):
        lo = int(np.searchsorted(rows, yk - r + 1, side="left"))
        hi = int(np.searchsorted(rows, yk + r, side="left"))
        n = hi - lo
        if n < design.min_windows:
            knots.append(Knot(yk, n, False, REASON_NO_WINDOWS, None, None))
            continue
        w0 = 1.0 - np.abs(rows[lo:hi] - yk) / r
        xs = x[lo:hi]
        fits = [_fit_channel(w0, xs, log_ratio[lo:hi, c], design) for c in range(3)]
        if any(f is None for f in fits):
            knots.append(Knot(yk, n, False, REASON_TRIMMED, None, None))
            continue
        r0 = np.array([f[0] for f in fits])
        r1 = np.array([f[1] for f in fits])
        beyond = bool((np.abs(r0 - log_global) > log_limit).any())
        knots.append(Knot(yk, n, not beyond, REASON_BEYOND_LIMIT if beyond else None, np.exp(r0), r1))
    return knots
