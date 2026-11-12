# a0seam: seam row-gain estimator

`a0seam` estimates, for each knot row along the seam of the stitched page, the gain that makes camera B' match camera A at the seam. It reads a request and writes a record. It never draws, saves or changes an image: applying the gains is the renderer's job (the C++ colour path), so the still render and the live render apply the same record with the same expression.

Runtime dependency: numpy only. OpenCV is not used. pytest is for the tests only.

## What it computes

Inputs are the strips of A and of B' around the seam, already mapped onto the output raster, and the global gain the renderer measured (B' x global gain is what the renderer applies today).

1. Flat windows. On a lattice of 8 px (anchored on the seam column, up to 96 px either side) a window is kept when the 45 x 45 neighbourhood is fully present in both strips and its luminance standard deviation is 3 DN or less in both cameras. The sample is the mean colour of the 21 x 21 neighbourhood in each camera. Any surface counts (paper, colour, dark), not only paper. A blurred edge raises the standard deviation inside the 45 x 45 neighbourhood, so windows next to it are dropped. Window means outside 1..254 DN are dropped as well (not in the design study, see Design constants).
2. Knots. A knot every 32 rows (and one on the last row). Per knot and colour channel, `log(A / B')` is fitted as `r0 + r1 * (x - seam) / 100 px` over the windows within 127 rows, with triangular weights, one trimming pass (residuals beyond 3 scaled MADs are dropped) and a refit. `exp(r0)` is the total gain at the seam; `r1` is recorded and not applied.
3. Unused knots. A knot is unused when it has fewer than 8 windows (`no-windows`), fewer than 8 remain after trimming (`too-few-after-trim`), or its gain differs from the global gain by more than a factor of 1.25 in any channel (`beyond-gain-limit`). Nothing is filled in from neighbouring knots. If a default per-row gain was given in the request, an unused knot carries the default at its row; otherwise its gain is `null`.
4. If more than a quarter of the knots are unused, the record is written with `state: "declined"` and must not be applied.

All tunables sit at the top of `a0seam/rowfield.py` and are written to the record. They are proposals from the design study, not approved values.

## Request: `a0.seam-estimate-request.v1`

A JSON file next to its data files (file names are relative to the request's folder, must stay inside it, and are checked by size and SHA-256).

```json
{
  "schema": "a0.seam-estimate-request.v1",
  "output": {"width": 4000, "height": 3000, "pixelPitchUm": 141.111, "originYMm": 0.0},
  "seamColumn": 2000,
  "globalGainBgr": [1.20, 1.18, 1.16],
  "strip": {"file": "strip.u16", "sha256": "<hex>", "firstColumn": 1500, "width": 1000},
  "defaultRowGain": {"file": "default-gain.f32", "sha256": "<hex>"},
  "design": {"stepPx": 8}
}
```

- `output`: the output raster in pixels; `pixelPitchUm` is the pixel size in micrometres (dpi = 25400 / pitch); `originYMm` is the document y of the top edge. Row `r` is at `originYMm + (r + 0.5) * pitch` mm.
- `seamColumn`: the seam column of the output raster.
- `globalGainBgr`: the global gain, order B, G, R, each in 0.01..100.
- `strip.file`: `height x width x 6` little-endian `uint16`, row-major. Channels are A (B, G, R) then B' (B, G, R); value = DN x 100; 65535 = no data. It must cover the seam column +- 118 columns (with the default design: 96 rounded down to the lattice, plus the 22 px flatness radius).
- `defaultRowGain` (optional): `height x 3` little-endian `float32`, the total per-row gain (B, G, R) the renderer would apply without this estimate.
- `design` (optional): overrides of the design constants, by the names the record uses (`stepPx`, `meanRadiusPx`, `flatRadiusPx`, `flatStdDn`, `seamHalfWidthPx`, `rowRadiusRows`, `knotSpacingRows`, `minWindows`, `trimMads`, `gainLimit`, `maxUnusedFraction`, `minLevelDn`, `maxLevelDn`). Unknown keys and out-of-range values are input errors.

Unknown keys anywhere, a wrong schema, wrong sizes, a hash mismatch or a path outside the folder are input errors.

## Record: `a0.seam-row-gain.v1`

`seam-row-gain.json`, UTF-8, LF, fixed key order, floats with 6 decimals. Main fields:

| field | meaning |
|---|---|
| `state` | `applied` or `declined` (`reason`: `too-many-unused-knots`) |
| `requestSha256` | SHA-256 of the request file, to detect a stale record |
| `environment` | Python, numpy and a0seam versions |
| `raster`, `globalGainBgr`, `design` | the request values and the constants actually used |
| `interpolation` | `log-linear`: between knots, interpolate the log of the gain linearly in `yMm`; hold the end knots' values outside them (the first and last knot are at the first and last row) |
| `windows` | `candidates` (lattice windows with a fully present neighbourhood) and `flat` (kept) |
| `knotSpacingMm`, `firstKnotMm` | nominal knot spacing and first knot |
| `knots[]` | `yMm`, `used`, `reason`, `windows`, `gainBgr` (total gain at the seam, B G R), `logSlopePerMm` |
| `unusedKnots`, `unusedByReason`, `gainRangeBgr` | counts, and min/max over used knots |

Knot positions are in millimetres and slopes are per millimetre, so the same record applies to any raster of the same page. The local ratio relative to the global gain is `gainBgr / globalGainBgr`.

## Running it

From the repository root, with a Python 3.12 that has the packages of `python/requirements.txt`:

```
python -m pip install -r python/requirements.txt
cd python
python -m a0seam.estimate <request.json> [--out <record.json>]
```

The default record path is `seam-row-gain.json` next to the request; it is written atomically. A host that starts Python in isolated mode (`-I`, which puts neither the current folder nor `PYTHONPATH` on `sys.path`) can use the launcher instead:

```
python -I python/estimate_seam_row_gain.py <request.json> [--out <record.json>]
```

Exit codes:

| code | meaning |
|---|---|
| 0 | record written, `state: applied` |
| 3 | record written, `state: declined`: do not apply it |
| 2 | the request or one of its files is not acceptable (also a command-line usage error); no record written |
| 1 | any other failure; no record written |

The one-line status goes to stdout, error text to stderr. A failed run does not delete an older record at the same path; compare `requestSha256`.

## Same input, same bytes

No random numbers, a fixed lattice, serial sums, thread pools pinned to one thread at import, and fixed-precision output. The record minus its `environment` block depends only on the request; the full file also names the Python and numpy versions. Bytes were compared across runs and thread settings on one machine; other CPUs and numpy versions are not covered (the 6-decimal output absorbs last-bit differences, and the tests pin a digest so a change shows up).

## Tests

```
python -m pip install -r python/requirements-dev.txt
cd python
python -m pytest
```

The tests build synthetic pages and cameras inside the test run (no photographs): recovery of a known row-varying gain with an x slope, surfaces that need different gains, a blurred band kept out of the windows, the gain limit and the declined state, knots without windows, input errors and exit codes, identical bytes and a pinned digest, and the same result from a half-resolution raster in millimetres.

## Not in this package

It does not draw images. A reference renderer (a numpy copy of the C++ expression) is only useful as a test oracle and is not part of this change.

## Third-party components

| component | licence | used for |
|---|---|---|
| numpy 2.5.4 | BSD-3-Clause (the wheel also bundles components under 0BSD, MIT, Zlib and CC0-1.0) | runtime |
| pytest 9.2.0 | MIT | tests only |
