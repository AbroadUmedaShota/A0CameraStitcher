"""Estimation on synthetic scenes: recovery, surfaces, blur, the gain limit, knots without windows."""
import numpy as np
import pytest

import synth
from a0seam import record as rec_mod

INTERIOR = (128.0, 512.0)     # reference rows whose knots have window support on both sides


def interior(ys):
    return (ys >= INTERIOR[0]) & (ys <= INTERIOR[1])


def knot_errors(rec, ramp=0.10):
    ys, gs = synth.knot_arrays(rec)
    return ys, gs / synth.true_gain(ys, 0.0, ramp) - 1.0


def seam_step(scene, rec, columns=16):
    """|A / (B' x gain) - 1| next to the seam on interior rows: with the estimate, and with the global gain alone."""
    strip = scene.strip().astype(np.float64) / 100.0
    a, b = strip[..., :3], strip[..., 3:]
    y_ref = (np.arange(scene.height) + 0.5) * scene.scale
    gain = synth.gain_at_reference_rows(rec, y_ref)
    cols = slice(scene.seam - columns, scene.seam + columns)
    rows = interior(y_ref)
    with_estimate = np.abs(a[:, cols] / (b[:, cols] * gain[:, None, :]) - 1.0)[rows]
    global_only = np.abs(a[:, cols] / (b[:, cols] * np.asarray(scene.global_gain)) - 1.0)[rows]
    return with_estimate, global_only


def row_of(rec, knot):
    return round((knot["yMm"] - rec["raster"]["originYMm"]) / (rec["raster"]["pixelPitchUm"] / 1000.0) - 0.5)


def gain_with_factor(y0, y1, factor):
    def fn(y, u):
        g = synth.true_gain(y, u)
        g[(y >= y0) & (y < y1)] *= factor
        return g
    return fn


def test_recovers_row_varying_gain_and_slope(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False)
    code, rec, _ = run_scene(scene)
    assert code == 0 and rec["state"] == "applied" and rec["unusedKnots"] == 0
    ys, err = knot_errors(rec)
    assert np.abs(err[interior(ys)]).max() <= 0.005
    pitch_mm = rec["raster"]["pixelPitchUm"] / 1000.0
    true_slope = synth.SLOPE_PER_100PX / (100.0 * pitch_mm)
    for knot in rec["knots"]:
        assert np.allclose(knot["logSlopePerMm"], true_slope, atol=2e-5)


def test_seam_step_after_correction(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False)
    _, rec, _ = run_scene(scene)
    after, before = seam_step(scene, rec)
    assert after.max() <= 0.005
    assert before.max() > 0.05      # the global gain alone leaves a clear step


def test_recovers_with_text_colour_and_dark_areas(run_scene):
    scene = synth.Scene()
    code, rec, _ = run_scene(scene)
    assert code == 0 and rec["unusedKnots"] == 0
    ys, err = knot_errors(rec)
    # The windows are not spread evenly over rows here (bars and blocks remove
    # some), which tilts the weighted mean of a knot; the limit is looser than
    # on the uniform page.
    assert np.abs(err[interior(ys)]).max() <= 0.015
    after, before = seam_step(scene, rec)
    assert after.max() <= 0.015 < before.max()


def test_surfaces_with_different_gain_give_a_value_between(run_scene):
    def page_fn(y, u):
        p = synth.page(y, u, bars=False, orange=False, dark=False)
        m = (y >= 200) & (y < 440) & (u > 0)
        p[m] = synth.ORANGE + synth._texture(y, u)[m][:, None]
        return p

    def gain_fn(y, u):
        g = synth.true_gain(y, u)
        g[(y >= 200) & (y < 440) & (u > 0)] *= 1.06    # the orange surface needs 6 % more
        return g

    code, rec, _ = run_scene(synth.Scene(page_fn=page_fn, gain_fn=gain_fn))
    assert code == 0
    ys, gs = synth.knot_arrays(rec)
    ratio = gs / synth.true_gain(ys, 0.0)
    inside = (ys >= 250) & (ys <= 400)
    assert (ratio[inside] > 1.01).all() and (ratio[inside] < 1.05).all()   # neither surface's own value
    outside = (ys <= 150) | (ys >= 500)
    assert np.abs(ratio[outside] - 1.0).max() < 0.02                       # rows with one surface only


@pytest.mark.parametrize("sigma", [4.0, 8.0])
def test_blurred_band_does_not_pull_the_estimate(run_scene, sigma):
    scene = synth.Scene(bars=False, orange=False, dark=False, band=(300, 340),
                        blur_a_sigma_ref=sigma, ramp=0.0)
    _, rec, _ = run_scene(scene)
    ys, err = knot_errors(rec, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() <= 0.0005


def test_without_the_flatness_limit_the_blur_does_pull(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, band=(300, 340), blur_a_sigma_ref=8.0,
                        ramp=0.0, design={"flatStdDn": 255.0})
    _, rec, _ = run_scene(scene)
    ys, err = knot_errors(rec, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() > 0.005


def test_knots_beyond_the_limit_return_to_the_default(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(230, 330, 3.0))
    code, rec, _ = run_scene(scene)
    assert code == 0 and rec["state"] == "applied"
    unused = [k for k in rec["knots"] if not k["used"]]
    assert rec["unusedKnots"] == len(unused) and 0 < len(unused) <= 0.25 * len(rec["knots"])
    assert rec["unusedByReason"]["beyond-gain-limit"] == len(unused)
    default = scene.default_row_gain()
    for k in unused:
        assert k["reason"] == "beyond-gain-limit"
        assert np.allclose(k["gainBgr"], default[row_of(rec, k)], atol=1e-6)
    assert {256, 288} <= {row_of(rec, k) for k in unused}
    # Knots far from the faulty rows are still used.
    far = [k for k in rec["knots"] if row_of(rec, k) <= 130 or row_of(rec, k) >= 450]
    assert far and all(k["used"] for k in far)


def test_too_many_unused_knots_decline_with_exit_code_3(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(150, 450, 2.0))
    code, rec, _ = run_scene(scene)
    assert code == 3
    assert rec["state"] == "declined" and rec["reason"] == "too-many-unused-knots"
    assert rec["unusedKnots"] > 0.25 * len(rec["knots"])


def test_knots_without_windows_are_not_filled_in(run_scene):
    def page_fn(y, u):
        p = synth.page(y, u, bars=False, orange=False, dark=False)
        p[(y >= 150) & (y < 330) & (np.mod(np.floor(u), 10) < 2)] = synth.INK
        return p

    scene = synth.Scene(page_fn=page_fn, design={"rowRadiusRows": 64})
    code, rec, _ = run_scene(scene)
    assert code == 0
    empty = [k for k in rec["knots"] if k["reason"] == "no-windows"]
    assert {row_of(rec, k) for k in empty} == {192, 224, 256, 288}
    default = scene.default_row_gain()
    for k in empty:
        assert k["used"] is False and k["windows"] == 0 and k["logSlopePerMm"] is None
        assert np.allclose(k["gainBgr"], default[row_of(rec, k)], atol=1e-6)
    assert rec["unusedByReason"]["no-windows"] == 4


def test_unused_knot_has_no_gain_when_no_default_was_given(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(230, 330, 3.0),
                        default_gain=False)
    _, rec, _ = run_scene(scene)
    unused = [k for k in rec["knots"] if not k["used"]]
    assert unused and all(k["gainBgr"] is None for k in unused)


def test_gain_range_covers_used_knots_only(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(230, 330, 3.0))
    _, rec, _ = run_scene(scene)
    used = np.array([k["gainBgr"] for k in rec["knots"] if k["used"]])
    assert np.allclose(rec["gainRangeBgr"]["min"], used.min(axis=0), atol=1e-6)
    assert np.allclose(rec["gainRangeBgr"]["max"], used.max(axis=0), atol=1e-6)


def test_record_states_the_design_that_was_used(run_scene):
    _, rec, _ = run_scene(synth.Scene(design={"gainLimit": 1.5}))
    assert rec["design"]["gainLimit"] == 1.5 and rec["design"]["stepPx"] == 8
    assert rec["schema"] == rec_mod.SCHEMA and rec["interpolation"] == "log-linear"


def test_a_knot_needs_the_minimum_number_of_windows(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, design={"minWindows": 500})
    _, rec, _ = run_scene(scene)
    counts = [k["windows"] for k in rec["knots"]]
    assert min(counts) < 500 <= max(counts)
    for k in rec["knots"]:
        assert k["used"] == (k["windows"] >= 500)
        assert k["reason"] in (None, "no-windows")


def test_a_patch_that_disagrees_with_the_rest_is_trimmed_away(run_scene):
    def gain_fn(y, u):
        g = synth.true_gain(y, u, 0.0)
        g[(y >= 290) & (y < 360) & (np.abs(u) < 35)] *= 1.15     # 70 x 70 patch right at the seam
        return g

    scene = synth.Scene(bars=False, orange=False, dark=False, ramp=0.0, gain_fn=gain_fn)
    code, rec, _ = run_scene(scene)
    assert code == 0 and rec["unusedKnots"] == 0
    ys, err = knot_errors(rec, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() <= 0.001
