"""Synthetic page, cameras and requests for the tests. No images from real captures, no random numbers.

The page is defined in reference pixels (a 640-row, 520-column raster with the
seam at column 260) and sampled on a raster scaled by 1/scale, so the same
scene can be fed at two resolutions. Camera A sees the page; camera B' sees the
page divided by the true gain, so the true gain is what the estimator must find.
"""
import hashlib
import json
import os
from dataclasses import dataclass, field

import numpy as np

DPI_REF = 180
PITCH_MM_REF = 25.4 / DPI_REF
TOP_UM = 12500
HEIGHT_REF = 640
WIDTH_REF = 520
SEAM_REF = 260

PAPER = np.array([226.0, 232.0, 236.0])      # B G R
ORANGE = np.array([40.0, 120.0, 220.0])
DARK = np.array([35.0, 40.0, 45.0])
INK = np.array([30.0, 30.0, 30.0])

GLOBAL_GAIN = np.array([1.20, 1.18, 1.16])
SLOPE_PER_100PX = np.array([-0.010, -0.012, -0.008])   # log gain per 100 reference pixels


def true_gain(y_ref, u_ref, ramp=0.10):
    """True per-channel gain of B' at reference row y and offset u from the seam (arrays broadcast)."""
    y = np.asarray(y_ref, dtype=np.float64)[..., None]
    u = np.asarray(u_ref, dtype=np.float64)[..., None]
    return GLOBAL_GAIN * (1.0 + ramp * (y - HEIGHT_REF / 2) / (HEIGHT_REF / 2)) * np.exp(SLOPE_PER_100PX * u / 100.0)


def _texture(y_ref, u_ref):
    """Deterministic fine texture, well under the flatness limit."""
    yi = np.floor(y_ref).astype(np.int64)
    ui = np.floor(u_ref).astype(np.int64)
    h = (yi * 7919 + ui * 104729) % 17
    return (h - 8.0) * 0.05


def page(y_ref, u_ref, bars=True, orange=True, dark=True, band=None):
    """Page colour (B G R) at reference coordinates."""
    y, u = np.broadcast_arrays(np.asarray(y_ref, dtype=np.float64), np.asarray(u_ref, dtype=np.float64))
    img = np.broadcast_to(PAPER, y.shape + (3,)).copy()
    if orange:
        m = (y >= 250) & (y < 330)
        img[m] = ORANGE
    if dark:
        m = (y >= 520) & (y < 600)
        img[m] = DARK
    if bars:
        m = (y >= 60) & (y < 100) & (np.mod(np.floor(u), 10) < 2)
        img[m] = INK
    if band is not None:     # (y0, y1): a wide dark band across the page
        m = (y >= band[0]) & (y < band[1])
        img[m] = INK
    return img + _texture(y, u)[..., None]


def gaussian_blur(img, sigma_px):
    if sigma_px <= 0:
        return img
    r = int(np.ceil(4 * sigma_px))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma_px) ** 2)
    k /= k.sum()
    out = img
    for axis in (0, 1):
        pad = [(0, 0)] * img.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="reflect")
        out = sum(k[i] * np.take(p, np.arange(i, i + out.shape[axis]), axis=axis) for i in range(2 * r + 1))
    return out


@dataclass
class Scene:
    scale: int = 1
    ramp: float = 0.10
    bars: bool = True
    orange: bool = True
    dark: bool = True
    band: object = None
    blur_a_sigma_ref: float = 0.0
    gain_fn: object = None        # optional replacement (y_ref, u_ref) -> (.., 3)
    page_fn: object = None        # optional replacement (y_ref, u_ref) -> (.., 3)
    design: dict = field(default_factory=dict)
    global_gain: tuple = tuple(GLOBAL_GAIN)
    b_samples: str = "mapped-before-color-gain"

    @property
    def height(self):
        return HEIGHT_REF // self.scale

    @property
    def width(self):
        return WIDTH_REF // self.scale

    @property
    def seam(self):
        return SEAM_REF // self.scale

    def coords(self):
        y_ref = (np.arange(self.height) + 0.5) * self.scale
        u_ref = (np.arange(self.width) + 0.5 - self.seam) * self.scale
        return np.meshgrid(y_ref, u_ref, indexing="ij")

    def gain(self, y_ref, u_ref):
        return self.gain_fn(y_ref, u_ref) if self.gain_fn else true_gain(y_ref, u_ref, self.ramp)

    def strip(self):
        y, u = self.coords()
        p = self.page_fn(y, u) if self.page_fn else page(y, u, self.bars, self.orange, self.dark, self.band)
        a = gaussian_blur(p, self.blur_a_sigma_ref / self.scale)
        b = p / self.gain(y, u)
        out = np.concatenate([a, b], axis=-1)
        return np.round(np.clip(out, 0.0, 255.0) * 100.0).astype("<u2")


def _sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def binding_list():
    """Three distinct, fixed fingerprints for the bindings of a request (no real files are fingerprinted)."""
    return [{"kind": kind, "sha256": hashlib.sha256(kind.encode("ascii")).hexdigest()}
            for kind in ("condition-v3", "capture-original-a", "capture-original-b")]


def request_doc(scene, strip_sha):
    """The request document of a scene (a dict); strip_sha is the SHA-256 of the strip file."""
    doc = {
        "schema": "a0.seam-estimate-request.v1",
        "raster": {"widthPx": scene.width, "heightPx": scene.height, "dpi": DPI_REF // scene.scale,
                   "topUm": TOP_UM, "seamColumn": scene.seam},
        "globalGainBgr": [float(v) for v in scene.global_gain],
        "bindings": binding_list(),
        "strip": {"file": "strip.u16", "sha256": strip_sha, "firstColumn": 0, "width": scene.width,
                  "bSamples": scene.b_samples},
    }
    if scene.design:
        doc["design"] = dict(scene.design)
    return doc


def write_request(folder, scene, name="request.json"):
    """Write the strip and the request JSON; returns the request path."""
    os.makedirs(folder, exist_ok=True)
    strip_path = os.path.join(folder, "strip.u16")
    scene.strip().tofile(strip_path)
    path = os.path.join(folder, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(request_doc(scene, _sha(strip_path)), f, indent=1)
    return path


# Design values that give the same millimetre footprint on a half-resolution raster.
HALF_SCALE_DESIGN = {
    "stepPx": 4, "meanRadiusPx": 5, "flatRadiusPx": 11, "seamHalfWidthPx": 48,
    "rowRadiusRows": 64, "knotSpacingRows": 16,
}


def knot_arrays(plan):
    """(rows in reference pixels, scales (n,3)) of the knots of a parsed plan."""
    raster = plan["diagnostics"]["raster"]
    top_mm = raster["topUm"] / 1000.0
    pitch_mm = 25.4 / raster["dpi"]
    scale = DPI_REF / raster["dpi"]
    ys = [((k["yMm"] - top_mm) / pitch_mm) * scale for k in plan["knots"]]
    return np.array(ys), np.array([k["seamScaleBgr"] for k in plan["knots"]])


def scale_at_reference_rows(plan, y_ref):
    """Scale between the knots of a plan as the reader defines it: linear in yMm, end values held."""
    ys, gs = knot_arrays(plan)
    return np.stack([np.interp(y_ref, ys, gs[:, c]) for c in range(3)], axis=-1)
