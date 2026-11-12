"""Reading and checking a seam estimate request (a0.seam-estimate-request.v1).

The request is a JSON file next to its data files. Every problem raises
InputError; nothing is guessed or repaired.

    {
      "schema": "a0.seam-estimate-request.v1",
      "output": {"width": 4000, "height": 3000, "pixelPitchUm": 141.111, "originYMm": 0.0},
      "seamColumn": 2000,
      "globalGainBgr": [1.20, 1.18, 1.16],
      "strip": {"file": "strip.u16", "sha256": "<hex>", "firstColumn": 1500, "width": 1000},
      "defaultRowGain": {"file": "default-gain.f32", "sha256": "<hex>"},   (optional)
      "design": {"stepPx": 8, ...}                                          (optional)
    }

strip file: rows x width x 6 little-endian uint16, row-major, channels A (B, G,
R) then B' (B, G, R); value = DN x 100; 65535 = no data.
defaultRowGain file: height x 3 little-endian float32, the total per-row gain
(B, G, R) the renderer would apply without this estimate.
"""
import hashlib
import json
import math
import os
from dataclasses import dataclass, replace

import numpy as np

from .errors import InputError
from .rowfield import DEFAULT_DESIGN, DESIGN_KEYS

SCHEMA = "a0.seam-estimate-request.v1"
MAX_REQUEST_BYTES = 1 << 20
MAX_DIMENSION = 100000
_TOP_KEYS = {"schema", "output", "seamColumn", "globalGainBgr", "strip", "defaultRowGain", "design"}
_INT_FIELDS = {"step_px", "mean_radius_px", "flat_radius_px", "seam_half_width_px",
               "row_radius_rows", "knot_spacing_rows", "min_windows"}


@dataclass(frozen=True)
class Request:
    sha256: str
    width: int
    height: int
    pixel_pitch_um: float
    origin_y_mm: float
    seam: int
    global_gain: tuple
    strip_path: str
    strip_first_column: int
    strip_width: int
    default_row_gain_path: object   # str or None
    design: object                  # rowfield.Design

    def read_strip_columns(self, first, stop):
        """Copy of the strip for output columns first..stop-1: (height, stop - first, 6) uint16."""
        view = np.memmap(self.strip_path, dtype="<u2", mode="r", shape=(self.height, self.strip_width, 6))
        try:
            return np.array(view[:, first - self.strip_first_column:stop - self.strip_first_column, :])
        finally:
            del view

    def load_default_row_gain(self):
        """(height, 3) float64 per-row default gain, or None."""
        if self.default_row_gain_path is None:
            return None
        raw = np.fromfile(self.default_row_gain_path, dtype="<f4")
        return raw.reshape(self.height, 3).astype(np.float64)


def _reject_constant(name):
    raise InputError(f"request contains {name}")


def _no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise InputError(f"request repeats the key {k!r}")
        out[k] = v
    return out


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _obj(parent, key, allowed):
    v = parent.get(key)
    if not isinstance(v, dict):
        raise InputError(f"{key} must be an object")
    extra = set(v) - allowed
    if extra:
        raise InputError(f"{key} has unknown keys: {sorted(extra)}")
    return v


def _sha_of_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _data_file(base_dir, spec, label, expected_size):
    """Resolve, size-check and hash-check a data file named in the request."""
    if not isinstance(spec.get("file"), str) or not spec["file"]:
        raise InputError(f"{label}.file must be a file name")
    name = spec["file"]
    if os.path.isabs(name) or os.path.splitdrive(name)[0]:
        raise InputError(f"{label}.file must be relative to the request")
    root = os.path.realpath(base_dir)
    path = os.path.realpath(os.path.join(root, name))
    try:
        inside = os.path.commonpath([root, path]) == root
    except ValueError:
        inside = False
    if not inside:
        raise InputError(f"{label}.file leaves the request folder")
    if not os.path.isfile(path):
        raise InputError(f"{label}.file does not exist")
    size = os.path.getsize(path)
    if size != expected_size:
        raise InputError(f"{label}.file has {size} bytes, expected {expected_size}")
    digest = spec.get("sha256")
    if not isinstance(digest, str) or len(digest) != 64 or digest != digest.lower() \
            or any(c not in "0123456789abcdef" for c in digest):
        raise InputError(f"{label}.sha256 must be 64 lowercase hex digits")
    if _sha_of_file(path) != digest:
        raise InputError(f"{label}.file does not match its sha256")
    return path


def _design(raw):
    if raw is None:
        return DEFAULT_DESIGN
    if not isinstance(raw, dict):
        raise InputError("design must be an object")
    changes = {}
    for key, value in raw.items():
        field = DESIGN_KEYS.get(key)
        if field is None:
            raise InputError(f"design has an unknown key {key!r}")
        if field in _INT_FIELDS:
            if not _is_int(value):
                raise InputError(f"design.{key} must be an integer")
        elif not _is_number(value):
            raise InputError(f"design.{key} must be a number")
        changes[field] = value if field in _INT_FIELDS else float(value)
    design = replace(DEFAULT_DESIGN, **changes)
    problems = design.problems()
    if problems:
        raise InputError("design: " + "; ".join(problems))
    return design


def load_request(path):
    """Read, validate and return a Request. Raises InputError for anything wrong."""
    try:
        size = os.path.getsize(path)
        if size > MAX_REQUEST_BYTES:
            raise InputError("request file is too large")
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise InputError(f"cannot read the request: {e.strerror or e}") from e
    try:
        doc = json.loads(raw.decode("utf-8"), object_pairs_hook=_no_duplicates, parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise InputError(f"request is not valid JSON: {e}") from e
    if not isinstance(doc, dict):
        raise InputError("request must be a JSON object")
    if doc.get("schema") != SCHEMA:
        raise InputError(f"schema must be {SCHEMA}")
    extra = set(doc) - _TOP_KEYS
    if extra:
        raise InputError(f"request has unknown keys: {sorted(extra)}")
    for key in ("output", "seamColumn", "globalGainBgr", "strip"):
        if key not in doc:
            raise InputError(f"{key} is missing")

    design = _design(doc.get("design"))

    out = _obj(doc, "output", {"width", "height", "pixelPitchUm", "originYMm"})
    for key in ("width", "height"):
        if not _is_int(out.get(key)) or not 1 <= out[key] <= MAX_DIMENSION:
            raise InputError(f"output.{key} must be an integer in 1..{MAX_DIMENSION}")
    width, height = out["width"], out["height"]
    pitch = out.get("pixelPitchUm")
    if not _is_number(pitch) or not 1.0 <= pitch <= 100000.0:
        raise InputError("output.pixelPitchUm must be a number in 1..100000")
    origin = out.get("originYMm")
    if not _is_number(origin) or abs(origin) > 1e6:
        raise InputError("output.originYMm must be a finite number")
    if height < 2 * design.flat_radius_px + 1:
        raise InputError("output.height is too small for the flatness window")

    seam = doc["seamColumn"]
    if not _is_int(seam) or not 0 <= seam < width:
        raise InputError("seamColumn must be an integer inside the output width")

    gain = doc["globalGainBgr"]
    if not isinstance(gain, list) or len(gain) != 3 or not all(_is_number(g) and 0.01 <= g <= 100.0 for g in gain):
        raise InputError("globalGainBgr must be three numbers in 0.01..100")

    s = _obj(doc, "strip", {"file", "sha256", "firstColumn", "width"})
    first, strip_width = s.get("firstColumn"), s.get("width")
    if not _is_int(first) or not _is_int(strip_width) or first < 0 or strip_width < 1 or first + strip_width > width:
        raise InputError("strip.firstColumn and strip.width must lie inside the output width")
    if first > seam - design.reach or first + strip_width - 1 < seam + design.reach:
        raise InputError(f"strip must cover the seam column +- {design.reach} columns")
    strip_path = _data_file(os.path.dirname(os.path.abspath(path)), s, "strip", height * strip_width * 6 * 2)

    default_path = None
    if doc.get("defaultRowGain") is not None:
        d = _obj(doc, "defaultRowGain", {"file", "sha256"})
        default_path = _data_file(os.path.dirname(os.path.abspath(path)), d, "defaultRowGain", height * 3 * 4)
        values = np.fromfile(default_path, dtype="<f4")
        if not np.isfinite(values).all() or (values <= 0).any():
            raise InputError("defaultRowGain must hold finite positive gains")

    return Request(
        sha256=hashlib.sha256(raw).hexdigest(), width=width, height=height,
        pixel_pitch_um=float(pitch), origin_y_mm=float(origin), seam=seam,
        global_gain=tuple(float(g) for g in gain), strip_path=strip_path,
        strip_first_column=first, strip_width=strip_width,
        default_row_gain_path=default_path, design=design)
