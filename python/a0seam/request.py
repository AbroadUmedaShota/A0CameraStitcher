"""Reading and checking a seam estimate request (a0.seam-estimate-request.v1).

The request is a JSON file next to its data files. Every problem raises
InputError; nothing is guessed or repaired.

    {
      "schema": "a0.seam-estimate-request.v1",
      "raster": {"widthPx": 4000, "heightPx": 3000, "dpi": 180, "topUm": 0, "seamColumn": 2000},
      "globalGainBgr": [1.20, 1.18, 1.16],
      "bindings": [{"kind": "condition-v3", "sha256": "<hex>"},
                   {"kind": "capture-original-a", "sha256": "<hex>"},
                   {"kind": "capture-original-b", "sha256": "<hex>"}],
      "strip": {"file": "strip.u16", "sha256": "<hex>", "firstColumn": 1500, "width": 1000,
                "bSamples": "mapped-before-color-gain"},
      "design": {"stepPx": 8, ...}                                          (development only)
    }

raster: the output raster; dpi and topUm (the top edge of the page region, in
micrometres) are integers, so row r is at yMm = topUm / 1000 + (r + 0.5) * 25.4
/ dpi.
bindings: facts the plan depends on, copied into the plan unchanged (this
program computes no SHA of them).
strip file: rows x width x 6 little-endian uint16, row-major, channels A (B, G,
R) then B' (B, G, R); value = DN x 100; 65535 = no data. B' is the original of
camera B mapped and sampled onto the output raster like the composite does,
with no colour scale applied (bSamples says so).
"""
import hashlib
import math
import os
from dataclasses import dataclass, replace

import numpy as np

from . import jsonio
from .errors import InputError
from .record import BINDING_KINDS
from .rowfield import DEFAULT_DESIGN, DESIGN_INT_FIELDS, DESIGN_KEYS, Design, knot_rows

SCHEMA = "a0.seam-estimate-request.v1"
MAX_REQUEST_BYTES = jsonio.MAX_JSON_BYTES
MAX_WIDTH_PX = 100000
MAX_HEIGHT_PX = 32768
MAX_STRIP_WIDTH = 1024
MAX_DPI = 4800
MAX_TOP_UM = 10**9
MAX_KNOTS = 4096           # the reader accepts at most this many knots
B_SAMPLES = "mapped-before-color-gain"
GLOBAL_GAIN_RANGE = (0.25, 4.0)   # the reader's gate on G
_TOP_KEYS = {"schema", "raster", "globalGainBgr", "bindings", "strip", "design"}
_RASTER_KEYS = ("widthPx", "heightPx", "dpi", "topUm", "seamColumn")
_STRIP_KEYS = {"file", "sha256", "firstColumn", "width", "bSamples"}


@dataclass(frozen=True)
class Request:
    sha256: str
    width: int
    height: int
    dpi: int
    top_um: int
    seam: int
    global_gain: tuple[float, float, float]        # B, G, R
    bindings: tuple[tuple[str, str], ...]          # (kind, sha256)
    strip_path: str
    strip_first_column: int
    strip_width: int
    design: Design

    @property
    def pitch_mm(self):
        return 25.4 / self.dpi

    def row_center_mm(self, row):
        """Document y (mm) of the centre of an output row."""
        return self.top_um / 1000.0 + (row + 0.5) * self.pitch_mm

    def read_strip_columns(self, first, stop):
        """Copy of the strip for output columns first..stop-1: (height, stop - first, 6) uint16."""
        view = np.memmap(self.strip_path, dtype="<u2", mode="r", shape=(self.height, self.strip_width, 6))
        try:
            return np.array(view[:, first - self.strip_first_column:stop - self.strip_first_column, :])
        finally:
            del view


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
        if field in DESIGN_INT_FIELDS:
            if not _is_int(value):
                raise InputError(f"design.{key} must be an integer")
        elif not _is_number(value):
            raise InputError(f"design.{key} must be a number")
        changes[field] = value if field in DESIGN_INT_FIELDS else float(value)
    design = replace(DEFAULT_DESIGN, **changes)
    problems = design.problems()
    if problems:
        raise InputError("design: " + "; ".join(problems))
    return design


def _in_range(v, low, high):
    return _is_int(v) and low <= v <= high


def read_raster(doc):
    """The raster entry: (widthPx, heightPx, dpi, topUm, seamColumn)."""
    r = _obj(doc, "raster", set(_RASTER_KEYS))
    missing = [k for k in _RASTER_KEYS if k not in r]
    if missing:
        raise InputError(f"raster is missing {missing}")
    if not _in_range(r["widthPx"], 1, MAX_WIDTH_PX):
        raise InputError(f"raster.widthPx must be an integer in 1..{MAX_WIDTH_PX}")
    if not _in_range(r["heightPx"], 1, MAX_HEIGHT_PX):
        raise InputError(f"raster.heightPx must be an integer in 1..{MAX_HEIGHT_PX}")
    if not _in_range(r["dpi"], 1, MAX_DPI):
        raise InputError(f"raster.dpi must be an integer in 1..{MAX_DPI}")
    if not _in_range(r["topUm"], -MAX_TOP_UM, MAX_TOP_UM):
        raise InputError(f"raster.topUm must be an integer within +-{MAX_TOP_UM}")
    if not _in_range(r["seamColumn"], 0, r["widthPx"] - 1):
        raise InputError("raster.seamColumn must be an integer inside the raster width")
    return tuple(r[k] for k in _RASTER_KEYS)


def read_bindings(doc):
    """The bindings entry as ((kind, sha256), ...). All kinds of a capture-estimate plan are required."""
    raw = doc.get("bindings")
    if not isinstance(raw, list) or not 1 <= len(raw) <= 8:
        raise InputError("bindings must be a list of 1..8 entries")
    out = []
    for i, item in enumerate(raw):
        if not isinstance(item, dict) or set(item) != {"kind", "sha256"}:
            raise InputError(f"bindings[{i}] must have exactly kind and sha256")
        if item["kind"] not in BINDING_KINDS:
            raise InputError(f"bindings[{i}].kind is not a known kind")
        if not jsonio.is_sha256(item["sha256"]):
            raise InputError(f"bindings[{i}].sha256 must be 64 lowercase hex digits")
        out.append((item["kind"], item["sha256"]))
    kinds = [k for k, _ in out]
    if len(set(kinds)) != len(kinds):
        raise InputError("bindings repeats a kind")
    absent = [k for k in BINDING_KINDS if k not in kinds]
    if absent:
        raise InputError(f"bindings lacks {absent}")
    return tuple(out)


def read_strip_spec(doc, base_dir, height, width, seam, reach):
    """Check the strip entry of a request and return (path, firstColumn, strip width).

    The strip holds A and B' (six channels) for all rows and the columns
    firstColumn .. firstColumn + width - 1 of an output raster of the given
    height and width; it must cover seam +- reach.
    """
    s = _obj(doc, "strip", _STRIP_KEYS)
    if s.get("bSamples") != B_SAMPLES:
        raise InputError(f"strip.bSamples must be {B_SAMPLES!r}")
    first, strip_width = s.get("firstColumn"), s.get("width")
    if not _is_int(first) or not _is_int(strip_width) or first < 0 or strip_width < 1 or first + strip_width > width:
        raise InputError("strip.firstColumn and strip.width must lie inside the raster width")
    if strip_width > MAX_STRIP_WIDTH:
        raise InputError(f"strip.width must be at most {MAX_STRIP_WIDTH}")
    if first > seam - reach or first + strip_width - 1 < seam + reach:
        raise InputError(f"strip must cover the seam column +- {reach} columns")
    path = jsonio.data_file(base_dir, s, "strip", height * strip_width * 6 * 2)
    return path, first, strip_width


def load_request(path):
    """Read, validate and return a Request. Raises InputError for anything wrong."""
    doc, raw = jsonio.read_json_file(path, "request", MAX_REQUEST_BYTES)
    if not isinstance(doc, dict):
        raise InputError("request must be a JSON object")
    if doc.get("schema") != SCHEMA:
        raise InputError(f"schema must be {SCHEMA}")
    extra = set(doc) - _TOP_KEYS
    if extra:
        raise InputError(f"request has unknown keys: {sorted(extra)}")
    for key in ("raster", "globalGainBgr", "bindings", "strip"):
        if key not in doc:
            raise InputError(f"{key} is missing")

    design = _design(doc.get("design"))
    width, height, dpi, top_um, seam = read_raster(doc)
    if height < 2 * design.flat_radius_px + 1:
        raise InputError("raster.heightPx is too small for the flatness window")
    if len(knot_rows(height, design)) > MAX_KNOTS:
        raise InputError(f"the raster would hold more than {MAX_KNOTS} knots")

    gain = doc["globalGainBgr"]
    low, high = GLOBAL_GAIN_RANGE
    if not isinstance(gain, list) or len(gain) != 3 or not all(_is_number(g) and low <= g <= high for g in gain):
        raise InputError(f"globalGainBgr must be three numbers in {low}..{high}")

    bindings = read_bindings(doc)
    base_dir = os.path.dirname(os.path.abspath(path))
    strip_path, first, strip_width = read_strip_spec(doc, base_dir, height, width, seam, design.reach)

    return Request(
        sha256=hashlib.sha256(raw).hexdigest(), width=width, height=height, dpi=dpi, top_um=top_um, seam=seam,
        global_gain=tuple(float(g) for g in gain), bindings=bindings, strip_path=strip_path,
        strip_first_column=first, strip_width=strip_width, design=design)
