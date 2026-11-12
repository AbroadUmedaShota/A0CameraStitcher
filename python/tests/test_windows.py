"""Flat window selection."""
import numpy as np

import synth
from a0seam.rowfield import DEFAULT_DESIGN
from a0seam.windows import MISSING, collect_flat_windows, lattice

SEAM = 150
HEIGHT = 200
WIDTH = 300
LEVEL_A = np.array([100.0, 150.0, 200.0])
LEVEL_B = np.array([80.0, 120.0, 160.0])


def flat_strip():
    """Uniform A and B' (values x 100), as an array to be edited."""
    a = np.broadcast_to(LEVEL_A, (HEIGHT, WIDTH, 3))
    b = np.broadcast_to(LEVEL_B, (HEIGHT, WIDTH, 3))
    return np.round(np.concatenate([a, b], axis=-1) * 100.0).astype("<u2")


def collect(strip, design=DEFAULT_DESIGN):
    return collect_flat_windows(strip, 0, SEAM, design)


def test_uniform_strip_gives_every_lattice_window_with_exact_means():
    found = collect(flat_strip())
    rows, cols = lattice(SEAM, HEIGHT, DEFAULT_DESIGN)
    assert found.rows.size == rows.size * cols.size == found.candidates
    assert np.allclose(found.mean_a, LEVEL_A) and np.allclose(found.mean_b, LEVEL_B)


def test_lattice_is_anchored_on_the_seam_and_ordered_row_major():
    found = collect(flat_strip())
    assert set((found.columns - SEAM) % DEFAULT_DESIGN.step_px) == {0}
    assert np.abs(found.columns - SEAM).max() <= DEFAULT_DESIGN.seam_half_width_px
    assert found.rows.min() == DEFAULT_DESIGN.flat_radius_px
    assert (np.diff(found.rows) >= 0).all()


def test_line_in_only_one_camera_removes_windows_around_it():
    for channel_offset in (0, 3):                      # the line is in A, then in B'
        strip = flat_strip()
        strip[:, 140:142, channel_offset:channel_offset + 3] = 3000
        found = collect(strip)
        r = DEFAULT_DESIGN.flat_radius_px
        assert not ((found.columns >= 140 - r) & (found.columns <= 141 + r)).any()
        assert (found.columns < 140 - r).any() and (found.columns > 141 + r).any()


def test_missing_data_removes_every_window_that_touches_it():
    strip = flat_strip()
    strip[100:104, 160:164, 4] = MISSING
    found = collect(strip)
    r = DEFAULT_DESIGN.flat_radius_px
    hit = (found.rows >= 100 - r) & (found.rows <= 103 + r) & (found.columns >= 160 - r) & (found.columns <= 163 + r)
    assert not hit.any()
    rows, cols = lattice(SEAM, HEIGHT, DEFAULT_DESIGN)
    assert found.candidates < rows.size * cols.size


def test_clipped_and_black_windows_are_not_used():
    strip = flat_strip()
    strip[:, 60:100, 0] = 25500        # A blue saturated over columns 60..99
    strip[:, 200:240, 4] = 0           # B' green black over columns 200..239
    found = collect(strip)
    r = DEFAULT_DESIGN.mean_radius_px
    assert not ((found.columns - r >= 60) & (found.columns + r < 100)).any()
    assert not ((found.columns - r >= 200) & (found.columns + r < 240)).any()
    assert np.isfinite(np.log(found.mean_a / found.mean_b)).all()


def test_blurred_band_is_kept_out_of_the_windows():
    scene = synth.Scene(bars=False, orange=False, dark=False, band=(300, 340), blur_a_sigma_ref=8.0)
    found = collect_flat_windows(scene.strip(), 0, scene.seam, DEFAULT_DESIGN)
    r = DEFAULT_DESIGN.flat_radius_px
    y_ref = found.rows + 0.5
    assert not ((y_ref + r > 300) & (y_ref - r < 340)).any()      # no window reaches the band itself
    assert (y_ref < 300 - r).any() and (y_ref > 340 + r).any()


def checker_strip(amplitude_dn, first_channel):
    """Uniform strip with a checkerboard of +-amplitude (luminance std = amplitude) in rows 60..119 of one camera."""
    values = flat_strip().astype(np.int32)
    checker = (np.arange(WIDTH)[None, :] + np.arange(HEIGHT)[:, None]) % 2 * 2 - 1
    values[60:120, :, first_channel:first_channel + 3] += (checker[60:120] * round(amplitude_dn * 100))[..., None]
    return values.astype("<u2")


def test_flatness_limit_is_the_luminance_standard_deviation_in_either_camera():
    r = DEFAULT_DESIGN.flat_radius_px
    for first_channel in (0, 3):
        inside = lambda f: (f.rows - r >= 60) & (f.rows + r < 120)    # window entirely within the textured rows
        rough = collect(checker_strip(5.0, first_channel))            # std 5 DN: above the 3 DN limit
        assert not inside(rough).any()
        smooth = collect(checker_strip(2.5, first_channel))           # std 2.5 DN: below it
        assert inside(smooth).any()
