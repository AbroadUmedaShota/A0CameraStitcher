"""Per-knot fit of the seam scale field.

For every knot row the scale B' needs to match A at the seam is estimated from
the flat windows around it: per colour channel, log(A / B') is fitted as
r0 + r1 * (x - seam) / 100 px with triangular row weights and one trimming
pass. exp(r0) is the scale at the seam (S, the global gain G included); the
slope r1 is only recorded.

A knot is unused when it has too few windows, when trimming leaves too few, or
when its scale is further from the global gain than the limit (S / G outside
[1 / r, r]). Unused knots are not filled in from neighbouring knots and do not
enter the plan. This module does not interpolate between knots.
"""
from dataclasses import dataclass, fields

import numpy as np

from .errors import need
from .windows import WINDOW_DESIGN_KEYS, WINDOW_INT_FIELDS, WindowDesign

# --- Fit constants ----------------------------------------------------------
# Proposals from the design study, not approved values. The window constants
# are in windows.py. The plan carries the values actually used, and a request
# may override them (see request.py).
ROW_RADIUS_ROWS = 128        # half width of the triangular row weights
KNOT_SPACING_ROWS = 32       # knot spacing (180 dpi: 4.52 mm)
MIN_WINDOWS = 8              # windows (and windows left after trimming) a knot needs
TRIM_MADS = 3.0              # residuals beyond this many scaled MADs are dropped once
MAX_SCALE_RATIO_TO_GLOBAL = 1.25   # S / G of a knot must stay within [1 / r, r]
MAX_UNUSED_FRACTION = 0.25   # more unused knots than this: the plan is declined
# The reader of the plan applies its own limit on S / G. This module must not
# write a knot the reader would refuse, so the tunable is capped at that value.
MAX_SCALE_RATIO_CAP = 1.25
# -----------------------------------------------------------------------------

_MAD_SCALE = 1.4826
_MAD_FLOOR = 0.002           # in log units; keeps the trim from collapsing on noise-free data


@dataclass(frozen=True)
class FitDesign:
    """The tunables of the per-knot fit."""
    row_radius_rows: int = ROW_RADIUS_ROWS
    knot_spacing_rows: int = KNOT_SPACING_ROWS
    min_windows: int = MIN_WINDOWS
    trim_mads: float = TRIM_MADS
    max_scale_ratio_to_global: float = MAX_SCALE_RATIO_TO_GLOBAL
    max_unused_fraction: float = MAX_UNUSED_FRACTION

    def fit_problems(self):
        """Reasons these fit tunables cannot be used; empty when acceptable."""
        out = []
        need(out, 1 <= self.row_radius_rows <= 8192, "rowRadiusRows must be 1..8192")
        need(out, 1 <= self.knot_spacing_rows <= 8192, "knotSpacingRows must be 1..8192")
        need(out, 3 <= self.min_windows <= 1000000, "minWindows must be at least 3")
        need(out, 0.0 < self.trim_mads <= 100.0, "trimMads must be positive")
        need(out, 1.0 < self.max_scale_ratio_to_global <= MAX_SCALE_RATIO_CAP,
             f"maxScaleRatioToGlobal must be above 1 and at most {MAX_SCALE_RATIO_CAP}")
        need(out, 0.0 <= self.max_unused_fraction <= 1.0, "maxUnusedFraction must be 0..1")
        return out


FIT_DESIGN_KEYS = {
    "rowRadiusRows": "row_radius_rows",
    "knotSpacingRows": "knot_spacing_rows",
    "minWindows": "min_windows",
    "trimMads": "trim_mads",
    "maxScaleRatioToGlobal": "max_scale_ratio_to_global",
    "maxUnusedFraction": "max_unused_fraction",
}
FIT_INT_FIELDS = {"row_radius_rows", "knot_spacing_rows", "min_windows"}


@dataclass(frozen=True)
class Design(FitDesign, WindowDesign):
    """Window and fit tunables of the seam estimate together."""

    def problems(self):
        """Reasons this design cannot be used; empty when it is acceptable."""
        return self.window_problems() + self.fit_problems()


DEFAULT_DESIGN = Design()

# JSON name -> Design field: the window tunables, then the fit tunables.
DESIGN_KEYS = {**WINDOW_DESIGN_KEYS, **FIT_DESIGN_KEYS}
DESIGN_INT_FIELDS = WINDOW_INT_FIELDS | FIT_INT_FIELDS
assert set(DESIGN_KEYS.values()) == {f.name for f in fields(Design)}

REASON_NO_WINDOWS = "no-windows"
REASON_TRIMMED = "too-few-after-trim"
REASON_BEYOND_LIMIT = "beyond-scale-limit"


@dataclass(frozen=True)
class ChannelFit:
    """The fit of one colour channel of one knot. Only kept is set when the fit failed."""
    ok: bool
    kept: int                       # windows left after trimming
    intercept: float | None = None  # r0
    slope: float | None = None      # r1 per 100 px
    effective: float | None = None  # (sum w) ** 2 / sum w ** 2 over the kept windows
    x_mean: float | None = None     # weighted mean of x, in units of 100 px
    x_spread: float | None = None   # weighted standard deviation of x, in units of 100 px
    mad_log: float | None = None    # scaled MAD of the final residuals, log units


@dataclass(frozen=True)
class Knot:
    row: int
    windows_in_range: int
    used: bool
    reason: str | None           # None when used
    scale: np.ndarray | None     # (3,) B G R scale S at the seam, exp(r0); None when not fitted or unused
    channels: tuple[ChannelFit, ...] | None   # three fits, or None when there were too few windows


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
    """One trimming pass and a refit. Returns a ChannelFit (ok is False when too few windows remain)."""
    a0, a1 = _wls_line(w0, x, y)
    e = y - a0 - a1 * x
    mad = np.median(np.abs(e)) * _MAD_SCALE + _MAD_FLOOR
    keep = np.abs(e) < design.trim_mads * mad
    kept = int(keep.sum())
    if kept < design.min_windows:
        return ChannelFit(False, kept)
    w = w0 * keep
    b0, b1 = _wls_line(w, x, y)
    sw = w.sum()
    x_mean = (w * x).sum() / sw
    return ChannelFit(
        True, kept, float(b0), float(b1),
        float(sw * sw / (w * w).sum()),
        float(x_mean), float(np.sqrt((w * (x - x_mean) ** 2).sum() / sw)),
        float(np.median(np.abs((y - b0 - b1 * x)[keep])) * _MAD_SCALE))


def fit_knots(windows, seam, global_gain, height, design):
    """Fit every knot. windows: windows.FlatWindows; global_gain: (3,) B G R."""
    order = np.argsort(windows.rows, kind="stable")
    rows = windows.rows[order]
    x = (windows.columns[order] - seam) / 100.0
    log_ratio = np.log(windows.mean_a[order] / windows.mean_b[order])
    log_global = np.log(np.asarray(global_gain, dtype=np.float64))
    log_limit = np.log(design.max_scale_ratio_to_global)
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
        fits = tuple(_fit_channel(w0, xs, log_ratio[lo:hi, c], design) for c in range(3))
        if not all(f.ok for f in fits):
            knots.append(Knot(yk, n, False, REASON_TRIMMED, None, fits))
            continue
        r0 = np.array([f.intercept for f in fits])
        beyond = bool((np.abs(r0 - log_global) > log_limit).any())
        knots.append(Knot(yk, n, not beyond, REASON_BEYOND_LIMIT if beyond else None, np.exp(r0), fits))
    return knots
