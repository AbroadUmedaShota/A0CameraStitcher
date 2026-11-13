"""The plan is in millimetres: a coarser raster of the same page gives the same result at the same places."""
import numpy as np

import synth


def diag_used(plan):
    return [k for k in plan["diagnostics"]["knots"] if k["used"]]


def test_half_resolution_input_gives_the_same_scales_at_the_same_millimetres(run_scene):
    full_code, full, _ = run_scene(synth.Scene(scale=1))
    half_code, half, _ = run_scene(synth.Scene(scale=2, design=synth.HALF_SCALE_DESIGN))
    assert full_code == half_code == 0
    assert full["diagnostics"]["unusedKnots"] == half["diagnostics"]["unusedKnots"] == 0

    # Same knot spacing in millimetres, and the dpi differs by the factor of two.
    spacing = [p["knots"][1]["yMm"] - p["knots"][0]["yMm"] for p in (full, half)]
    assert abs(spacing[0] - spacing[1]) < 1e-5
    assert full["diagnostics"]["raster"]["dpi"] == 2 * half["diagnostics"]["raster"]["dpi"]

    rows = np.linspace(150.0, 500.0, 36)               # reference rows, i.e. the same places in mm
    a, b = synth.scale_at_reference_rows(full, rows), synth.scale_at_reference_rows(half, rows)
    assert np.abs(a / b - 1.0).max() <= 0.005
    # And both agree with the truth on a page whose gain is smooth.
    truth = synth.true_gain(rows, 0.0)
    assert np.abs(a / truth - 1.0).max() <= 0.015 and np.abs(b / truth - 1.0).max() <= 0.015


def test_slope_is_per_millimetre_on_both_rasters(run_scene):
    _, full, _ = run_scene(synth.Scene(scale=1, bars=False, orange=False, dark=False))
    _, half, _ = run_scene(synth.Scene(scale=2, bars=False, orange=False, dark=False,
                                       design=synth.HALF_SCALE_DESIGN))
    s_full = np.array([k["logSlopePerMm"] for k in diag_used(full)])
    s_half = np.array([k["logSlopePerMm"] for k in diag_used(half)])
    assert np.allclose(np.median(s_full, axis=0), np.median(s_half, axis=0), atol=5e-5)
