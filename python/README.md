# a0seam: seam colour plan estimator

`a0seam` estimates, for knots along the seam of the stitched page, the colour scale that makes camera B' match camera A at the seam. It reads a request and writes a plan, `a0.seam-color-plan.v1`. It never draws, saves or changes an image: applying the plan is the renderer's job (the C++ colour path), so the still render and the live render apply the same plan with the same expression.

## Where it fits

`a0seam` is a tool for calibration and for development. A product capture and the live preview do not start it, and the application does not bundle Python or numpy. A PC that runs it has Python 3.12 and the packages of `requirements.txt` installed. Without them the application still captures; the seam colour is then the one the renderer measures itself.

Runtime dependency: numpy only. OpenCV is not used. pytest is for the tests only.

## What it computes

Inputs are the strips of A and of B' around the seam, already mapped onto the output raster, and the global gain G the renderer measured.

**B' is the original of camera B, mapped onto the output raster and sampled with the same mapping and bilinear sampling as the composite, with no colour scale applied.** Neither G nor any seam scale is multiplied in. The request says so (`strip.bSamples`). Passing an already corrected B' gives a plan that looks valid and is wrong, so the estimator also checks it (step 4).

1. Flat windows. On a lattice of 8 px (anchored on the seam column, up to 96 px either side) a window is kept when the 45 x 45 neighbourhood is fully present in both strips and its luminance standard deviation is 3 DN or less in both cameras. The sample is the mean colour of the 21 x 21 neighbourhood in each camera. Any surface counts (paper, colour, dark), not only paper. A blurred edge raises the standard deviation inside the 45 x 45 neighbourhood, so windows next to it are dropped. Window means outside 1..254 DN are dropped as well.
2. Knots. A knot every 32 rows (and one on the last row). Per knot and colour channel, `log(A / B')` is fitted as `r0 + r1 * (x - seam) / 100 px` over the windows within 127 rows, with triangular weights, one trimming pass (residuals beyond 3 scaled MADs are dropped) and a refit. `exp(r0)` is the scale S at the seam; `r1` is recorded and not applied.
3. Knots left out. A knot is not used when it has fewer than 8 windows (`no-windows`), fewer than 8 remain after trimming (`too-few-after-trim`), or its S differs from G by more than a factor of 1.25 in any channel (`beyond-scale-limit`). A knot that is not used is not in the plan, is not replaced by a neighbour's value or by any default, and is listed with its reason under `diagnostics`.
4. Declined. The plan is written with `state: "declined"` and no knots, and must not be applied, when
   - the median of A / B' over all flat windows is more than 8 % away from G in any channel (`strip-ratio-far-from-global`; the ratio is in `diagnostics.stripRatioToGlobalBgr`). This catches a B' that already carries G, which would otherwise give S near 1 inside the scale limit. It cannot catch it when G is within about 8 % of 1. The 8 % is a proposal;
   - more than a quarter of the knots are not used (`too-many-unused-knots`);
   - fewer than two knots are used (`too-few-knots`).

Window and fit constants are at the top of `a0seam/windows.py` and `a0seam/rowfield.py`. They are proposals from the design study, not approved values, and the plan records the values used.

## Names

| name | where | meaning |
|---|---|---|
| `globalGainBgr` (G) | plan | the global gain: multiplies all of B' and stays to the right edge. Order B, G, R |
| `seamScaleBgr` (S) | plan | the total scale of B' at the seam, **G included**. Not a ratio to G |
| `seamOffsetBgr` | plan | additive term in linear light; always 0 in space `gamma8` |
| S / G | reader only | the part beyond G. It is computed where the plan is applied and is never written to the file |
| `maxScaleRatioToGlobal` | design constant, `diagnostics.design` | a knot is left out when S / G leaves `[1 / r, r]`. Default 1.25, which is also the largest value accepted. It was called `gainLimit`; it is unrelated to the limit of the C++ colour match, which is an absolute limit on a measured gain |

## Request: `a0.seam-estimate-request.v1`

A JSON file next to its data files (file names are relative to the request's folder, must stay inside it, and are checked by size and SHA-256).

```json
{
  "schema": "a0.seam-estimate-request.v1",
  "raster": {"widthPx": 4000, "heightPx": 3000, "dpi": 180, "topUm": 0, "seamColumn": 2000},
  "globalGainBgr": [1.20, 1.18, 1.16],
  "bindings": [
    {"kind": "condition-v3", "sha256": "<64 hex>"},
    {"kind": "capture-original-a", "sha256": "<64 hex>"},
    {"kind": "capture-original-b", "sha256": "<64 hex>"}
  ],
  "strip": {"file": "strip.u16", "sha256": "<64 hex>", "firstColumn": 1500, "width": 1000,
            "bSamples": "mapped-before-color-gain"}
}
```

- `raster`: the output raster. `dpi` and `topUm` (the top edge of the page region, in micrometres) are integers. Row `r` is at `yMm = topUm / 1000 + (r + 0.5) * 25.4 / dpi`, the expression the renderer uses. `heightPx` is at most 32768.
- `globalGainBgr`: G, order B, G, R, each in 0.25..4.
- `bindings`: the facts the plan depends on (v1: the SHA-256 of the valid condition revision and of the two capture originals). They are written by the program that makes the request and copied unchanged into the plan; `a0seam` computes no SHA of them.
- `strip.file`: `height x width x 6` little-endian `uint16`, row-major. Channels are A (B, G, R) then B' (B, G, R); value = DN x 100; 65535 = no data. `width` is at most 1024. It must cover the seam column +- 118 columns (with the default design: 96 rounded down to the lattice, plus the 22 px flatness radius). Rows x columns around the seam may not exceed 4 million pixels.
- `design` (optional, development only): overrides of the constants, by the names the plan records (`stepPx`, `meanRadiusPx`, `flatRadiusPx`, `flatStdDn`, `seamHalfWidthPx`, `minLevelDn`, `maxLevelDn`, `rowRadiusRows`, `knotSpacingRows`, `minWindows`, `trimMads`, `maxScaleRatioToGlobal`, `maxUnusedFraction`). A product request leaves it out; the values used are written to `diagnostics.design`.

Unknown keys anywhere, a wrong schema, wrong sizes, a hash mismatch, a path outside the folder, a nesting that is too deep or an integer of more than 18 digits are input errors.

## Plan: `a0.seam-color-plan.v1`

`seam-color-plan.json`, UTF-8, no BOM, LF, fixed key order, floats with 6 decimals. A plan with synthetic values:

```json
{
  "schema": "a0.seam-color-plan.v1",
  "source": "capture-estimate",
  "state": "usable",
  "reason": null,
  "space": "gamma8",
  "bindings": [{"kind": "condition-v3", "sha256": "<64 hex>"}, "..."],
  "requestSha256": "<64 hex>",
  "globalGainBgr": [1.100000, 1.080000, 1.060000],
  "knots": [
    {"yMm": 10.000000, "seamScaleBgr": [1.111000, 1.085400, 1.054700], "seamOffsetBgr": [0.000000, 0.000000, 0.000000]}
  ],
  "diagnostics": {}
}
```

(The real file spreads each object over several lines. A complete synthetic plan is `tests/fixtures/plan-synthetic.json`.)

| field | meaning |
|---|---|
| `source` | `capture-estimate`: estimated from one capture |
| `state`, `reason` | `usable` with `reason: null`, or `declined` with one of the reasons of step 4 |
| `space` | `gamma8`: the scales multiply 8-bit values |
| `bindings` | copied from the request. The plan applies only when each of them equals the fact it names at the time of use |
| `requestSha256` | SHA-256 of the request file, to tell which request a plan answers |
| `globalGainBgr` | G |
| `knots[]` | the knots that are used, in increasing `yMm`: S and the offset |
| `diagnostics` | the estimator's evidence. Not part of the contract and not to be interpreted; see below |

**Between knots** the reader interpolates `seamScaleBgr` and `seamOffsetBgr` linearly in `yMm` and holds the first and last knot's values outside them. The estimator does not interpolate, and the plan has no field for the rule. A knot that is not in the plan is bridged by that rule, not by a default.

`diagnostics` holds: `model`, `environment` (Python, numpy and a0seam versions), `raster`, `design` (the constants used), `windows` (candidates and kept), `stripRatioToGlobalBgr`, the knot counts (`knotCount`, `usedKnots`, `unusedKnots`, `unusedByReason`), `longestGapMm` (the longest stretch covered by a run of unused knots, measured from the used knot before it to the one after; 0 when all are used) and `knots[]`, one entry for every knot with `yMm`, `used`, `reason`, `windowsInRange`, `windows` (kept after trimming, per channel), `effectiveCount` ((sum of weights)^2 / sum of squared weights), `xMeanMm` and `xSpreadMm` (weighted mean and standard deviation of the window offsets from the seam, in mm, signed), `residualMadLogBgr` (scaled MAD of the final residuals), `logSlopePerMm` and `seamScaleBgr` (null when the knot is not used). Values the fit did not produce are null. These fields can change without a new schema version.

## Running it

From the repository root, with a Python 3.12 that has the packages of `python/requirements.txt`:

```
python -m pip install -r python/requirements.txt
cd python
python -m a0seam.estimate <request.json> [--out <plan.json>]
```

The default plan path is `seam-color-plan.json` next to the request; it is written atomically (flushed, then renamed over the old file). A host that starts Python in isolated mode (`-I`, which puts neither the current folder nor `PYTHONPATH` on `sys.path`) uses the launcher instead:

```
python -I -X utf8 python/estimate_seam_row_gain.py --out <plan.json> -- <request.json>
```

Exit codes:

| code | meaning |
|---|---|
| 0 | plan written, `state: usable` |
| 3 | plan written, `state: declined`: do not apply it |
| 2 | the request or one of its files is not acceptable (also a command-line usage error); no plan written |
| 1 | any other failure; no plan written |

The one-line status goes to stdout and error text to stderr. Neither contains a file system path, and both replace characters the console's code page cannot show, so the exit code does not depend on the code page. A run that writes no plan leaves a file already at the output path as it was.

## For the program that starts it

- Start the interpreter by its absolute path from the settings, not by searching `PATH`, and without a shell. The arguments are the options shown above, the plan path and the request path, nothing else. Put `--` before the request path so that a path beginning with `-` is not read as an option.
- Use `-I -X utf8`. `-I` ignores `PYTHONPATH`, `PYTHONUTF8` and `PYTHONIOENCODING`, so an environment variable cannot do what `-X utf8` does.
- Give every run a new, empty folder for the request, the strip and the plan, and delete any plan at the output path before the run. A failed run does not remove an older file, so a plan found after a failure may be a leftover.
- Give the run a deadline, and treat a timeout like exit code 1.
- Read the plan only when the exit code is 0, and then check that `requestSha256` equals the SHA-256 of the request file you wrote, that `source`, `space` and `state` are known, and that every `bindings` entry equals the fact you hold. Exit code 3 is not an error of the program: keep the renderer's own colour. Any other code, or a failed check, does the same.
- Do not interpret `diagnostics`.
- `yMm` is exact only to 6 decimals; do not compare it with raster rows for equality.

## Same input, same bytes

No random numbers, a fixed lattice, sums that numpy does in a fixed order (pairwise, not split across threads), thread pools pinned to one thread by the launchers, and fixed-precision output. Importing the package changes nothing in the process. The plan minus `diagnostics.environment` depends only on the request. Bytes were compared across runs and thread settings on one machine; other CPUs and numpy versions are not covered (the 6-decimal output absorbs last-bit differences, and the tests pin a digest so a change shows up).

## Tests

```
python -m pip install -r python/requirements-dev.txt
cd python
python -m pytest
```

The tests build synthetic pages and cameras inside the test run (no photographs): recovery of a known row-varying scale with an x slope, surfaces that need different scales, a blurred band kept out of the windows, the scale limit and the declined states, knots without windows, a B' that already carries G, input errors and exit codes, what a failed run leaves behind, identical bytes and a pinned digest, and the same result from a half-resolution raster in millimetres. `tests/plan_contract.py` holds the reader's rules for a plan, and `tests/fixtures/plan-synthetic.json` is a hand-made plan that passes them.

## Not in this package

It does not draw images and does not interpolate. It does not estimate from white and black reference captures; that estimator will be added here and write the same plan format.

## Third-party components

| component | licence | used for |
|---|---|---|
| numpy 2.5.4 | BSD-3-Clause. The Windows wheel also contains code under other licences, listed in the wheel's `numpy-2.5.4.dist-info/licenses/LICENSE.txt`: OpenBLAS (BSD-3-Clause), LAPACK (BSD-3-Clause-Open-MPI), libgfortran (GPL-3.0-or-later WITH GCC-exception-3.1, linked statically into the OpenBLAS DLL), and parts under 0BSD, MIT, Zlib, Apache-2.0, NCSA and CC0-1.0. This repository does not include numpy; `pip` installs it. | runtime |
| pytest 9.2.0 | MIT | tests only |

pytest's own dependencies, also tests only: colorama 0.4.6 (BSD-3-Clause), iniconfig 2.3.1 (MIT), packaging 26.3 (Apache-2.0 OR BSD-2-Clause), pluggy 1.7.0 (MIT), Pygments 2.21.0 (BSD-2-Clause).

Nothing in this table is redistributed by this repository. Shipping Python or numpy with the application is a separate decision (HG-0005) and needs their licence files to travel with them.
