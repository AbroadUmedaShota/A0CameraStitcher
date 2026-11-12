"""The record is in millimetres: a coarser raster of the same page gives the same result at the same places."""
import numpy as np

import synth


def gains_at_common_rows(rec, rows):
    return synth.gain_at_reference_rows(rec, rows)


def test_half_resolution_input_gives_the_same_gains_at_the_same_millimetres(run_scene):
    full_code, full, _ = run_scene(synth.Scene(scale=1))
    half_code, half, _ = run_scene(synth.Scene(scale=2, design=synth.HALF_SCALE_DESIGN))
    assert full_code == half_code == 0
    assert full["unusedKnots"] == half["unusedKnots"] == 0

    # Same knot spacing in millimetres, same size of the page in millimetres.
    assert abs(full["knotSpacingMm"] - half["knotSpacingMm"]) < 1e-5
    assert abs(full["raster"]["pixelPitchUm"] * 2 - half["raster"]["pixelPitchUm"]) < 1e-5

    rows = np.linspace(150.0, 500.0, 36)               # reference rows, i.e. the same places in mm
    a, b = gains_at_common_rows(full, rows), gains_at_common_rows(half, rows)
    assert np.abs(a / b - 1.0).max() <= 0.005
    # And both agree with the truth on a page whose gain is smooth.
    truth = synth.true_gain(rows, 0.0)
    assert np.abs(a / truth - 1.0).max() <= 0.015 and np.abs(b / truth - 1.0).max() <= 0.015


def test_slope_is_per_millimetre_on_both_rasters(run_scene):
    _, full, _ = run_scene(synth.Scene(scale=1, bars=False, orange=False, dark=False))
    _, half, _ = run_scene(synth.Scene(scale=2, bars=False, orange=False, dark=False,
                                       design=synth.HALF_SCALE_DESIGN))
    s_full = np.array([k["logSlopePerMm"] for k in full["knots"] if k["used"]])
    s_half = np.array([k["logSlopePerMm"] for k in half["knots"] if k["used"]])
    assert np.allclose(np.median(s_full, axis=0), np.median(s_half, axis=0), atol=5e-5)
