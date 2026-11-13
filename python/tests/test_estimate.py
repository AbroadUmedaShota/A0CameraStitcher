"""Estimation on synthetic scenes: recovery, surfaces, blur, the scale limit, knots without windows."""
import numpy as np
import pytest

import synth
from a0seam import record as rec_mod

INTERIOR = (128.0, 512.0)     # reference rows whose knots have window support on both sides


def interior(ys):
    return (ys >= INTERIOR[0]) & (ys <= INTERIOR[1])


def knot_errors(plan, ramp=0.10):
    ys, gs = synth.knot_arrays(plan)
    return ys, gs / synth.true_gain(ys, 0.0, ramp) - 1.0


def seam_step(scene, plan, columns=16):
    """|A / (B' x scale) - 1| next to the seam on interior rows: with the plan, and with the global gain alone."""
    strip = scene.strip().astype(np.float64) / 100.0
    a, b = strip[..., :3], strip[..., 3:]
    y_ref = (np.arange(scene.height) + 0.5) * scene.scale
    scale = synth.scale_at_reference_rows(plan, y_ref)
    cols = slice(scene.seam - columns, scene.seam + columns)
    rows = interior(y_ref)
    with_plan = np.abs(a[:, cols] / (b[:, cols] * scale[:, None, :]) - 1.0)[rows]
    global_only = np.abs(a[:, cols] / (b[:, cols] * np.asarray(scene.global_gain)) - 1.0)[rows]
    return with_plan, global_only


def diag_knots(plan):
    return plan["diagnostics"]["knots"]


def row_of(plan, y_mm):
    raster = plan["diagnostics"]["raster"]
    return round((y_mm - raster["topUm"] / 1000.0) / (25.4 / raster["dpi"]) - 0.5)


def plan_rows(plan):
    return {row_of(plan, k["yMm"]) for k in plan["knots"]}


def gain_with_factor(y0, y1, factor):
    def fn(y, u):
        g = synth.true_gain(y, u)
        g[(y >= y0) & (y < y1)] *= factor
        return g
    return fn


def test_recovers_row_varying_scale_and_slope(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False)
    code, plan, _ = run_scene(scene)
    assert code == 0 and plan["state"] == "usable" and plan["diagnostics"]["unusedKnots"] == 0
    ys, err = knot_errors(plan)
    assert np.abs(err[interior(ys)]).max() <= 0.005
    pitch_mm = 25.4 / plan["diagnostics"]["raster"]["dpi"]
    true_slope = synth.SLOPE_PER_100PX / (100.0 * pitch_mm)
    for knot in diag_knots(plan):
        assert np.allclose(knot["logSlopePerMm"], true_slope, atol=2e-5)


def test_seam_step_after_correction(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False)
    _, plan, _ = run_scene(scene)
    after, before = seam_step(scene, plan)
    assert after.max() <= 0.005
    assert before.max() > 0.05      # the global gain alone leaves a clear step


def test_recovers_with_text_colour_and_dark_areas(run_scene):
    scene = synth.Scene()
    code, plan, _ = run_scene(scene)
    assert code == 0 and plan["diagnostics"]["unusedKnots"] == 0
    ys, err = knot_errors(plan)
    # The windows are not spread evenly over rows here (bars and blocks remove
    # some), which tilts the weighted mean of a knot; the limit is looser than
    # on the uniform page.
    assert np.abs(err[interior(ys)]).max() <= 0.015
    after, before = seam_step(scene, plan)
    assert after.max() <= 0.015 < before.max()


def test_surfaces_with_different_scale_give_a_value_between(run_scene):
    def page_fn(y, u):
        p = synth.page(y, u, bars=False, orange=False, dark=False)
        m = (y >= 200) & (y < 440) & (u > 0)
        p[m] = synth.ORANGE + synth._texture(y, u)[m][:, None]
        return p

    def gain_fn(y, u):
        g = synth.true_gain(y, u)
        g[(y >= 200) & (y < 440) & (u > 0)] *= 1.06    # the orange surface needs 6 % more
        return g

    code, plan, _ = run_scene(synth.Scene(page_fn=page_fn, gain_fn=gain_fn))
    assert code == 0
    ys, gs = synth.knot_arrays(plan)
    ratio = gs / synth.true_gain(ys, 0.0)
    inside = (ys >= 250) & (ys <= 400)
    assert (ratio[inside] > 1.01).all() and (ratio[inside] < 1.05).all()   # neither surface's own value
    outside = (ys <= 150) | (ys >= 500)
    assert np.abs(ratio[outside] - 1.0).max() < 0.02                       # rows with one surface only


@pytest.mark.parametrize("sigma", [4.0, 8.0])
def test_blurred_band_does_not_pull_the_estimate(run_scene, sigma):
    scene = synth.Scene(bars=False, orange=False, dark=False, band=(300, 340),
                        blur_a_sigma_ref=sigma, ramp=0.0)
    _, plan, _ = run_scene(scene)
    ys, err = knot_errors(plan, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() <= 0.0005


def test_without_the_flatness_limit_the_blur_does_pull(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, band=(300, 340), blur_a_sigma_ref=8.0,
                        ramp=0.0, design={"flatStdDn": 255.0})
    _, plan, _ = run_scene(scene)
    ys, err = knot_errors(plan, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() > 0.005


def test_knots_beyond_the_scale_limit_are_left_out_of_the_plan(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(230, 330, 3.0))
    code, plan, _ = run_scene(scene)
    assert code == 0 and plan["state"] == "usable"
    d = plan["diagnostics"]
    unused = [k for k in diag_knots(plan) if not k["used"]]
    assert d["unusedKnots"] == len(unused) and 0 < len(unused) <= 0.25 * d["knotCount"]
    assert d["unusedByReason"]["beyond-scale-limit"] == len(unused)
    assert len(plan["knots"]) == d["knotCount"] - len(unused) == d["usedKnots"]
    left_out = {row_of(plan, k["yMm"]) for k in unused}
    assert {256, 288} <= left_out
    assert not left_out & plan_rows(plan)               # no knot is filled in from a default or a neighbour
    for k in unused:
        assert k["reason"] == "beyond-scale-limit" and k["seamScaleBgr"] is None
    # Knots far from the faulty rows are still used.
    far = [k for k in diag_knots(plan) if row_of(plan, k["yMm"]) <= 130 or row_of(plan, k["yMm"]) >= 450]
    assert far and all(k["used"] for k in far)


def test_too_many_unused_knots_decline_with_exit_code_3(run_scene):
    def gain_fn(y, u):          # a low band at the top and a high band at the bottom: the median of A / B' stays put
        g = synth.true_gain(y, u)
        g[y < 200] *= 0.7
        g[y >= 440] *= 1.4
        return g

    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_fn)
    code, plan, _ = run_scene(scene)
    assert code == 3
    assert plan["state"] == "declined" and plan["reason"] == "too-many-unused-knots"
    assert plan["knots"] == []
    assert plan["diagnostics"]["unusedKnots"] > 0.25 * plan["diagnostics"]["knotCount"]


def test_fewer_than_two_usable_knots_decline(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False,
                        design={"minWindows": 1000000, "maxUnusedFraction": 1.0})
    code, plan, _ = run_scene(scene)
    assert code == 3
    assert plan["state"] == "declined" and plan["reason"] == "too-few-knots"
    assert plan["diagnostics"]["usedKnots"] < 2 and plan["knots"] == []


def test_knots_without_windows_are_not_in_the_plan(run_scene):
    def page_fn(y, u):
        p = synth.page(y, u, bars=False, orange=False, dark=False)
        p[(y >= 150) & (y < 330) & (np.mod(np.floor(u), 10) < 2)] = synth.INK
        return p

    scene = synth.Scene(page_fn=page_fn, design={"rowRadiusRows": 64})
    code, plan, _ = run_scene(scene)
    assert code == 0
    empty = [k for k in diag_knots(plan) if k["reason"] == "no-windows"]
    assert {row_of(plan, k["yMm"]) for k in empty} == {192, 224, 256, 288}
    for k in empty:
        assert k["used"] is False and k["windowsInRange"] == 0
        assert k["windows"] is None and k["logSlopePerMm"] is None and k["seamScaleBgr"] is None
    assert plan["diagnostics"]["unusedByReason"]["no-windows"] == 4
    assert not {192, 224, 256, 288} & plan_rows(plan)


def test_longest_gap_spans_the_run_of_left_out_knots(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, gain_fn=gain_with_factor(230, 330, 3.0))
    _, plan, _ = run_scene(scene)
    knots = diag_knots(plan)
    unused = [i for i, k in enumerate(knots) if not k["used"]]
    assert unused == list(range(unused[0], unused[-1] + 1))             # one run, so one gap to check
    before, after = knots[unused[0] - 1]["yMm"], knots[unused[-1] + 1]["yMm"]
    assert abs(plan["diagnostics"]["longestGapMm"] - (after - before)) < 1e-5
    _, clean, _ = run_scene(synth.Scene(bars=False, orange=False, dark=False))
    assert clean["diagnostics"]["longestGapMm"] == 0.0


def test_knot_quality_is_recorded_for_every_fitted_knot(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, ramp=0.0)
    _, plan, _ = run_scene(scene)
    pitch_mm = 25.4 / plan["diagnostics"]["raster"]["dpi"]
    reach_mm = 96 * pitch_mm
    for k in diag_knots(plan):
        assert k["used"] and k["windowsInRange"] >= max(k["windows"]) >= min(k["windows"]) >= 8
        for c in range(3):
            assert 0.0 < k["effectiveCount"][c] <= k["windows"][c]
            assert abs(k["xMeanMm"][c]) < reach_mm and 0.0 < k["xSpreadMm"][c] < reach_mm
            assert 0.0 <= k["residualMadLogBgr"][c] < 0.003      # the page is flat in y here
    # The knots at the page edges see windows on one side only, which shows in a lower effective count.
    counts = [min(k["effectiveCount"]) for k in diag_knots(plan)]
    assert counts[0] < counts[len(counts) // 2] > counts[-1]
    # A gain that changes along y inside the knot's rows is what the residual spread is there to show.
    _, tilted, _ = run_scene(synth.Scene(bars=False, orange=False, dark=False, ramp=0.10))
    flat_mad = np.median([k["residualMadLogBgr"] for k in diag_knots(plan)])
    tilted_mad = np.median([k["residualMadLogBgr"] for k in diag_knots(tilted)])
    assert tilted_mad > 5 * flat_mad


def test_diagnostics_state_the_design_that_was_used(run_scene):
    _, plan, _ = run_scene(synth.Scene(design={"maxScaleRatioToGlobal": 1.1}))
    design = plan["diagnostics"]["design"]
    assert design["maxScaleRatioToGlobal"] == 1.1 and design["stepPx"] == 8
    assert "gainLimit" not in design
    assert plan["schema"] == rec_mod.SCHEMA == "a0.seam-color-plan.v1"


def test_a_knot_needs_the_minimum_number_of_windows(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False, design={"minWindows": 500})
    _, plan, _ = run_scene(scene)
    counts = [k["windowsInRange"] for k in diag_knots(plan)]
    assert min(counts) < 500 <= max(counts)
    for k in diag_knots(plan):
        assert k["used"] == (k["windowsInRange"] >= 500)
        assert k["reason"] in (None, "no-windows")


def test_a_patch_that_disagrees_with_the_rest_is_trimmed_away(run_scene):
    def gain_fn(y, u):
        g = synth.true_gain(y, u, 0.0)
        g[(y >= 290) & (y < 360) & (np.abs(u) < 35)] *= 1.15     # 70 x 70 patch right at the seam
        return g

    scene = synth.Scene(bars=False, orange=False, dark=False, ramp=0.0, gain_fn=gain_fn)
    code, plan, _ = run_scene(scene)
    assert code == 0 and plan["diagnostics"]["unusedKnots"] == 0
    ys, err = knot_errors(plan, ramp=0.0)
    assert np.abs(err[interior(ys)]).max() <= 0.001
