"""Flat windows: places where both cameras see a smooth surface.

A window centre is kept when the 45 x 45 neighbourhood (flat radius) is fully
present in both strips and has a luminance standard deviation at or below the
limit in both cameras. The mean colour of the 21 x 21 neighbourhood (mean
radius) of each camera is then the sample. Blur does not change the mean of a
uniform surface, but near an edge it raises the standard deviation inside the
flat radius, so the window is rejected there.

No randomness; windows come out in row-major order of the fixed lattice.
"""
from dataclasses import dataclass

import numpy as np

from .errors import need

# --- Window constants -------------------------------------------------------
# Proposals from the design study, not approved values. The plan carries the
# values actually used, and a request may override them (see request.py).
STEP_PX = 8                  # lattice step of window centres
MEAN_RADIUS_PX = 10          # half width of the colour mean window (21 x 21)
FLAT_RADIUS_PX = 22          # half width of the flatness window (45 x 45)
FLAT_STD_DN = 3.0            # luminance standard deviation limit inside the flatness window
SEAM_HALF_WIDTH_PX = 96      # windows are taken at most this far from the seam column
MIN_LEVEL_DN = 1.0           # guard (not in the design): a window mean below this cannot give a ratio
MAX_LEVEL_DN = 254.0         # guard (not in the design): a window mean above this is clipped
# -----------------------------------------------------------------------------

MISSING = 65535
_LUMA_BGR = (0.114, 0.587, 0.299)
_LUMA_CENTRE = 128.0


@dataclass(frozen=True)
class WindowDesign:
    """The tunables of the window search."""
    step_px: int = STEP_PX
    mean_radius_px: int = MEAN_RADIUS_PX
    flat_radius_px: int = FLAT_RADIUS_PX
    flat_std_dn: float = FLAT_STD_DN
    seam_half_width_px: int = SEAM_HALF_WIDTH_PX
    min_level_dn: float = MIN_LEVEL_DN
    max_level_dn: float = MAX_LEVEL_DN

    @property
    def reach(self):
        """How far from the seam column the strip must extend on each side."""
        return (self.seam_half_width_px // self.step_px) * self.step_px + self.flat_radius_px

    def window_problems(self):
        """Reasons these window tunables cannot be used; empty when acceptable."""
        out = []
        need(out, 1 <= self.step_px <= 1024, "stepPx must be 1..1024")
        need(out, 1 <= self.mean_radius_px <= self.flat_radius_px <= 1024,
             "radii must satisfy 1 <= meanRadiusPx <= flatRadiusPx <= 1024")
        need(out, 0 <= self.seam_half_width_px <= 4096, "seamHalfWidthPx must be 0..4096")
        need(out, 0.0 <= self.flat_std_dn <= 255.0, "flatStdDn must be 0..255")
        need(out, 0.0 < self.min_level_dn < self.max_level_dn <= 255.0,
             "level guards must satisfy 0 < minLevelDn < maxLevelDn <= 255")
        return out


# JSON name -> WindowDesign field.
WINDOW_DESIGN_KEYS = {
    "stepPx": "step_px",
    "meanRadiusPx": "mean_radius_px",
    "flatRadiusPx": "flat_radius_px",
    "flatStdDn": "flat_std_dn",
    "seamHalfWidthPx": "seam_half_width_px",
    "minLevelDn": "min_level_dn",
    "maxLevelDn": "max_level_dn",
}
WINDOW_INT_FIELDS = {"step_px", "mean_radius_px", "flat_radius_px", "seam_half_width_px"}


@dataclass(frozen=True)
class FlatWindows:
    """Samples of one request: window centres (output pixels) and camera means (DN)."""
    rows: np.ndarray       # (n,) int64
    columns: np.ndarray    # (n,) int64
    mean_a: np.ndarray     # (n, 3) float64, B G R
    mean_b: np.ndarray     # (n, 3) float64, B G R
    candidates: int        # lattice windows whose flat neighbourhood is fully present in both strips


def _integral(q):
    s = np.zeros((q.shape[0] + 1, q.shape[1] + 1), dtype=np.float64)
    s[1:, 1:] = q.cumsum(axis=0).cumsum(axis=1)
    return s


def _box(s, rows, cols, r):
    y0, y1 = (rows - r)[:, None], (rows + r + 1)[:, None]
    x0, x1 = (cols - r)[None, :], (cols + r + 1)[None, :]
    return s[y1, x1] - s[y0, x1] - s[y1, x0] + s[y0, x0]


def lattice(seam, height, design):
    """Window centres: absolute output rows and columns."""
    rows = np.arange(design.flat_radius_px, height - design.flat_radius_px, design.step_px, dtype=np.int64)
    k = design.seam_half_width_px // design.step_px
    cols = seam + design.step_px * np.arange(-k, k + 1, dtype=np.int64)
    return rows, cols


def collect_flat_windows(strip, first_column, seam, design):
    """Find the flat windows of a strip.

    strip: (H, W, 6) uint16, values x 100, MISSING where unavailable; channels
    are A (B, G, R) then B' (B, G, R). first_column: output column of strip[:, 0].
    The strip must cover seam +- design.reach.
    """
    height = strip.shape[0]
    rows, cols = lattice(seam, height, design)
    if rows.size == 0:
        return FlatWindows(np.empty(0, np.int64), np.empty(0, np.int64), np.empty((0, 3)), np.empty((0, 3)), 0)
    c0 = int(cols[0]) - design.flat_radius_px
    c1 = int(cols[-1]) + design.flat_radius_px + 1
    sub = np.asarray(strip[:, c0 - first_column:c1 - first_column, :])
    cols_rel = cols - c0

    valid = (sub != MISSING).all(axis=2)
    vals = np.where(valid[..., None], sub.astype(np.float64) / 100.0, 0.0)

    r_flat, r_mean = design.flat_radius_px, design.mean_radius_px
    n_flat, n_mean = (2 * r_flat + 1) ** 2, (2 * r_mean + 1) ** 2
    full = _box(_integral(valid.astype(np.float64)), rows, cols_rel, r_flat) >= n_flat - 0.5

    def luminance_sd(first_channel):
        lum = sum(w * vals[..., first_channel + i] for i, w in enumerate(_LUMA_BGR)) - _LUMA_CENTRE
        lum = np.where(valid, lum, 0.0)
        m1 = _box(_integral(lum), rows, cols_rel, r_flat) / n_flat
        m2 = _box(_integral(lum * lum), rows, cols_rel, r_flat) / n_flat
        return np.sqrt(np.maximum(0.0, m2 - m1 * m1))

    ok = full & (luminance_sd(0) <= design.flat_std_dn) & (luminance_sd(3) <= design.flat_std_dn)
    means = np.stack([_box(_integral(vals[..., c]), rows, cols_rel, r_mean) / n_mean for c in range(6)], axis=-1)
    ok &= ((means >= design.min_level_dn) & (means <= design.max_level_dn)).all(axis=-1)

    iy, ix = np.nonzero(ok)
    sel = means[iy, ix]
    return FlatWindows(rows[iy], cols[ix], sel[:, :3], sel[:, 3:], int(full.sum()))
