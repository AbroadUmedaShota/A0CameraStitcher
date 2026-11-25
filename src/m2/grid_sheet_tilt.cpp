#include "a0/m2/experimental/grid_sheet_tilt.hpp"

#include "a0/m2/experimental/content_registration.hpp"
#include "grid_sheet_tilt_detail.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace a0::m2::experimental::grid_sheet_tilt {
namespace {

using render::Point;

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
double Wrap180(double degrees) {
    double v = std::fmod(degrees, 360.0);
    if (v <= -180.0) v += 360.0;
    if (v > 180.0) v -= 360.0;
    return v;
}
Point Rotate(Point v, double degrees) {
    const double c = std::cos(degrees * detail::kDegrees), s = std::sin(degrees * detail::kDegrees);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}
double Distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool Finite(double v) { return std::isfinite(v); }
bool FinitePositive(double v) { return std::isfinite(v) && v > 0; }
bool FiniteNonNegative(double v) { return std::isfinite(v) && v >= 0; }

void ValidateProfile(const GridSheetProfile& profile) {
    const auto& i = profile.projection.intrinsics;
    Require(FinitePositive(i.fx_pixels) && FinitePositive(i.fy_pixels) && Finite(i.cx_pixels) && Finite(i.cy_pixels),
        "grid sheet tilt: the projection has no valid intrinsics");
    Require(profile.picture_width >= 16 && profile.picture_height >= 16 && profile.picture_width <= 32768 && profile.picture_height <= 32768,
        "grid sheet tilt: the profile's picture size is not set");
    // A principal point outside the middle half of the profile's own picture means the size and the projection do not
    // belong together (a projection of another resolution); no lens puts it there.
    const double w = profile.picture_width, h = profile.picture_height;
    Require(i.cx_pixels >= 0.25 * w && i.cx_pixels <= 0.75 * w && i.cy_pixels >= 0.25 * h && i.cy_pixels <= 0.75 * h,
        "grid sheet tilt: the profile's principal point is not in the middle of its picture size");
}

void ValidateSettings(const GridSheetTiltSettings& s) {
    const auto& d = s.detection;
    Require(FinitePositive(d.pitch_min_px) && FinitePositive(d.pitch_max_px) && d.pitch_min_px > 4 && d.pitch_max_px > d.pitch_min_px,
        "grid sheet tilt: the pitch range is not set");
    Require(FinitePositive(d.window_half_size) && d.window_min_points >= 1 && d.direction_min_points >= 1 && d.lattice_min_points >= 1 &&
        d.fit_max_points >= 100 && d.fit_iterations >= 1 && d.fit_min_inliers >= 9 && d.line_min_points >= 3 && d.node_min_count >= 8 &&
        FinitePositive(d.node_max_gap_px) && d.coverage_cell_px >= 1 && d.coverage_cell_min_points >= 1 && d.flat_cells_across >= 1 &&
        d.flat_cell_min_points >= 1 && Finite(d.flat_cell_coherence) && d.flat_cell_coherence > 0 && d.flat_cell_coherence < 1,
        "grid sheet tilt: the detection settings are not set");
    Require(Finite(d.window_min_score) && d.window_min_score > 0 && d.window_min_score <= 1 && Finite(d.second_direction_window_deg) &&
        d.second_direction_window_deg > 0 && d.second_direction_window_deg <= 45,
        "grid sheet tilt: the first lattice settings are not set");
    Require(Finite(d.pitch_coarse_divisor) && d.pitch_coarse_divisor >= 4 && FinitePositive(d.pitch_fine_step) && FinitePositive(d.pitch_fine_span) &&
        d.pitch_fine_span < 1 && d.window_scan_points >= 50 && d.init_scan_points >= 100,
        "grid sheet tilt: the pitch scan settings are not set");
    Require(Finite(d.direction_gate_deg) && d.direction_gate_deg > 0 && d.direction_gate_deg < 45 && Finite(d.direction_match_deg) &&
        d.direction_match_deg > 0 && d.direction_match_deg < 45 && FinitePositive(d.final_gate_px) && FinitePositive(d.fit_settle_px),
        "grid sheet tilt: the gates are not set");
    double previous_radius = 0;
    for (std::size_t stage = 0; stage < d.radii.size(); ++stage) {
        Require(FinitePositive(d.radii[stage]) && d.radii[stage] > previous_radius, "grid sheet tilt: the fit radii do not grow");
        previous_radius = d.radii[stage];
        Require(FinitePositive(d.gates_px[stage][0]), "grid sheet tilt: a fit stage has no gate");
        for (double gate : d.gates_px[stage]) Require(FiniteNonNegative(gate), "grid sheet tilt: a fit gate is not set");
    }
    const auto& a = s.acceptance;
    Require(FiniteNonNegative(a.max_residual_rms_px) && FiniteNonNegative(a.max_method_spread_deg) && FiniteNonNegative(a.min_visible_fraction) &&
        FiniteNonNegative(a.max_off_lattice_cell_fraction) && FiniteNonNegative(a.off_lattice_offset_px) && FiniteNonNegative(a.max_lattice_aspect_deviation) &&
        FiniteNonNegative(a.max_lattice_skew_deg) && a.min_lines_per_family >= 2, "grid sheet tilt: the acceptance is not set");
    // The rms of the points inside the final gate stays near final_gate / sqrt(3) even when they lie anywhere in it (they
    // are cut off by the gate), so a residual line at or above that never refuses anything.
    Require(a.max_residual_rms_px < d.final_gate_px / std::sqrt(3.0), "grid sheet tilt: the residual line is beyond what the final gate lets through");
}

double LeanDistance(const detail::OrientationNumbers& a, const detail::OrientationNumbers& b) { return Distance(a.lean_sheet_deg, b.lean_sheet_deg); }

}   // namespace

ImageView ViewOf(const render::BgrImage& image) {
    ImageView v;
    v.width = image.width; v.height = image.height; v.format = PixelFormat::bgr8; v.pixels = image.bgr.data();
    v.stride_bytes = static_cast<std::size_t>(image.width) * 3;
    if (image.bgr.size() < static_cast<std::size_t>(image.width) * image.height * 3) throw std::invalid_argument("grid sheet tilt: the picture buffer is shorter than its size");
    return v;
}

GridSheetTiltSettings WorkingDraftSettings() {
    GridSheetTiltSettings s;
    // Working numbers for development. They only decide when a measurement is given up and why; none of them is an
    // accepted optical threshold or a statement about what tilt is acceptable. The method spread is a disagreement
    // between three computations on the same points, not a tolerance on the tilt.
    s.acceptance.min_ridge_points = 20000;
    s.acceptance.min_lines_per_family = 20;
    s.acceptance.min_lattice_span = 20;
    s.acceptance.min_visible_fraction = 0.3;          // the picture is of the sheet itself, not of a drawing on it
    s.acceptance.max_lattice_aspect_deviation = 0.02;
    s.acceptance.max_lattice_skew_deg = 1.0;
    s.acceptance.max_residual_rms_px = 0.8;           // below the final gate's 2.5 / sqrt(3)
    s.acceptance.max_method_spread_deg = 0.3;
    s.acceptance.max_off_lattice_cell_fraction = 0.15;
    s.acceptance.off_lattice_offset_px = 4.0;
    s.pair.max_rotation_mismatch_deg = 2.0;
    return s;
}

std::string_view RefusalCode(GridSheetRefusal refusal) {
    switch (refusal) {
    case GridSheetRefusal::none: return "none";
    case GridSheetRefusal::picture_not_profile_size: return "picture-not-profile-size";
    case GridSheetRefusal::too_few_ridge_points: return "too-few-ridge-points";
    case GridSheetRefusal::no_grid_found: return "no-grid-found";
    case GridSheetRefusal::fit_failed: return "fit-failed";
    case GridSheetRefusal::too_few_lines: return "too-few-lines";
    case GridSheetRefusal::span_too_small: return "span-too-small";
    case GridSheetRefusal::view_too_small: return "view-too-small";
    case GridSheetRefusal::grid_not_square: return "grid-not-square";
    case GridSheetRefusal::residual_too_large: return "residual-too-large";
    case GridSheetRefusal::flatness_not_judged: return "flatness-not-judged";
    case GridSheetRefusal::sheet_not_flat: return "sheet-not-flat";
    case GridSheetRefusal::methods_disagree: return "methods-disagree";
    }
    throw std::invalid_argument("unknown grid sheet refusal");
}

std::string_view RefusalCode(GridSheetPairRefusal refusal) {
    switch (refusal) {
    case GridSheetPairRefusal::none: return "none";
    case GridSheetPairRefusal::reference_not_measured: return "reference-not-measured";
    case GridSheetPairRefusal::target_not_measured: return "target-not-measured";
    case GridSheetPairRefusal::turn_mismatch: return "turn-mismatch";
    }
    throw std::invalid_argument("unknown grid sheet pair refusal");
}

GridSheetTiltOutcome MeasureGridSheetTilt(std::string_view camera, const ImageView& picture, const GridSheetProfile& profile,
    const GridSheetTiltSettings& settings) {
    Require(picture.pixels != nullptr && picture.width >= 16 && picture.height >= 16, "grid sheet tilt: the picture is empty or too small");
    ValidateProfile(profile);
    ValidateSettings(settings);
    const auto& projection = profile.projection;
    const auto& det = settings.detection;
    const auto& acc = settings.acceptance;
    GridSheetTiltOutcome out;
    out.refusal = GridSheetRefusal::fit_failed;
    auto& ev = out.evidence;
    const auto refuse = [&](GridSheetRefusal why) { out.refusal = why; out.tilt.reset(); return out; };

    // The calibrated turn of the image axes: a programming error when the profile itself is not a valid projection.
    const double calibration_azimuth = content_registration::OrientationOf(projection).rotation_clockwise_deg;

    // Only the profile's own picture size: the pixel numbers of the projection hold for that size alone.
    if (picture.width != profile.picture_width || picture.height != profile.picture_height) return refuse(GridSheetRefusal::picture_not_profile_size);

    // (a) lines
    const detail::RidgeResult ridge = detail::DetectRidges(picture, det);
    ev.ridge_points = ridge.points.size();
    if (ridge.points.size() < std::max<std::size_t>(acc.min_ridge_points, 1)) return refuse(GridSheetRefusal::too_few_ridge_points);

    // (c) points without the lens distortion
    const detail::Lens lens = detail::LensOf(projection);
    const double focal = lens.MeanFocal();
    const detail::PreparedPoints points = detail::PreparePoints(ridge.points, lens, true);

    // (b) first lattice
    const auto initial = detail::FindInitialLattice(points, focal, det);
    if (!initial) return refuse(GridSheetRefusal::no_grid_found);
    ev.pitch_px = initial->pitch_px;

    // (d) the lattice
    const detail::LatticeFit fit = detail::FitLattice(points, focal, *initial, det);
    ev.fit_converged = fit.converged;
    ev.fit_last_step_px = fit.last_step_px;
    if (!fit.ok || !fit.converged) return refuse(GridSheetRefusal::fit_failed);
    const detail::Mat3 h = detail::LatticeToPlane(fit.parameters);
    if (!detail::PositivelyOriented(h)) return refuse(GridSheetRefusal::fit_failed);
    const detail::LatticeShape shape = detail::ShapeOfLattice(h);
    ev.lattice_aspect_ratio = shape.aspect_ratio;
    ev.lattice_skew_deg = shape.skew_deg;

    const detail::Assignment assignment = detail::AssignPoints(fit.parameters, points, focal, det.direction_match_deg, det.final_gate_px);
    double sum_sq = 0, max_distance_px = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!assignment.ok[i]) continue;
        ++ev.used_points;
        sum_sq += static_cast<double>(assignment.distance_px[i]) * assignment.distance_px[i];
        max_distance_px = std::max(max_distance_px, static_cast<double>(assignment.distance_px[i]));
    }
    ev.used_point_fraction = static_cast<double>(ev.used_points) / static_cast<double>(ev.ridge_points);
    if (ev.used_points == 0) return refuse(GridSheetRefusal::fit_failed);
    ev.residual_rms_px = std::sqrt(sum_sq / static_cast<double>(ev.used_points));
    ev.visible_fraction = detail::VisibleFraction(ridge.points, points, assignment, picture.width, picture.height, det.coverage_cell_px, det.coverage_cell_min_points);

    // (e) the tilt, three ways
    const detail::OrientationNumbers main = detail::OrientationOfLattice(projection, h);
    if (!main.ok) return refuse(GridSheetRefusal::fit_failed);
    ev.tilt_deg_fit = main.tilt_deg;
    const auto lines = detail::FitLines(points, assignment, focal, det.line_min_points);
    // The span counts lattice lines from the first to the last fitted line (a line with enough points), so a few stray
    // points far out do not widen it.
    std::array<std::int64_t, 2> lowest{INT64_MAX, INT64_MAX}, highest{INT64_MIN, INT64_MIN};
    for (const auto& l : lines) {
        const auto f = static_cast<std::size_t>(l.family);
        ++ev.lines[f];
        lowest[f] = std::min<std::int64_t>(lowest[f], l.index); highest[f] = std::max<std::int64_t>(highest[f], l.index);
    }
    for (std::size_t f = 0; f < 2; ++f) ev.lattice_span[f] = ev.lines[f] ? static_cast<std::int32_t>(highest[f] - lowest[f] + 1) : 0;
    const detail::NodeFit nodes = detail::FitNodes(lines, h, focal, det);
    ev.nodes = nodes.nodes;
    detail::OrientationNumbers by_nodes;
    if (nodes.ok) {
        ev.node_residual_rms_px = nodes.rms_px;
        by_nodes = detail::OrientationOfLattice(projection, nodes.lattice_to_plane);
        if (by_nodes.ok) ev.tilt_deg_nodes = by_nodes.tilt_deg;
    }
    const detail::VanishingFit vanishing = detail::FitVanishing(lines, h);
    if (vanishing.ok) ev.tilt_deg_vanishing = vanishing.tilt_deg;
    if (nodes.ok && by_nodes.ok && vanishing.ok) {
        detail::OrientationNumbers v;
        v.lean_sheet_deg = vanishing.lean_sheet_deg;
        ev.method_spread_deg = std::max({LeanDistance(main, by_nodes), LeanDistance(main, v), LeanDistance(by_nodes, v)});
    }
    ev.tilt_formal_sigma_deg = detail::FormalTiltSigma(fit.parameters, points, focal, projection, det);
    ev.half_split_spread_deg = detail::HalfSplitSpread(fit.parameters, points, ridge.points, assignment, focal, projection, det);
    const detail::FlatnessCheck flat = detail::CheckFlatness(fit.parameters, points, ridge.points, picture.width, picture.height, focal, acc.off_lattice_offset_px, det);
    ev.off_lattice_cell_fraction = flat.off_lattice_fraction;
    ev.judged_cells = flat.judged_cells;

    // The caller's lines decide whether the value is shown. Each comparison is written so that a value that was not
    // reached (NaN) refuses.
    if (ev.lines[0] < acc.min_lines_per_family || ev.lines[1] < acc.min_lines_per_family || !nodes.ok || !by_nodes.ok || !vanishing.ok)
        return refuse(GridSheetRefusal::too_few_lines);
    if (ev.lattice_span[0] < static_cast<std::int64_t>(acc.min_lattice_span) || ev.lattice_span[1] < static_cast<std::int64_t>(acc.min_lattice_span))
        return refuse(GridSheetRefusal::span_too_small);
    if (!(ev.visible_fraction >= acc.min_visible_fraction)) return refuse(GridSheetRefusal::view_too_small);
    if (!(std::abs(ev.lattice_aspect_ratio - 1.0) <= acc.max_lattice_aspect_deviation) || !(std::abs(ev.lattice_skew_deg) <= acc.max_lattice_skew_deg))
        return refuse(GridSheetRefusal::grid_not_square);
    if (!(ev.residual_rms_px <= acc.max_residual_rms_px)) return refuse(GridSheetRefusal::residual_too_large);
    if (flat.judged_cells == 0) return refuse(GridSheetRefusal::flatness_not_judged);
    if (!(ev.off_lattice_cell_fraction <= acc.max_off_lattice_cell_fraction)) return refuse(GridSheetRefusal::sheet_not_flat);
    if (!(ev.method_spread_deg <= acc.max_method_spread_deg)) return refuse(GridSheetRefusal::methods_disagree);

    GridSheetTilt tilt;
    tilt.camera = std::string(camera);
    tilt.tilt_deg = main.tilt_deg;
    tilt.lean_sheet_deg = main.lean_sheet_deg;
    tilt.image_x_azimuth_deg = main.image_x_azimuth_deg;
    tilt.calibration_x_azimuth_deg = calibration_azimuth;
    tilt.lean_screen_deg = Rotate(main.lean_sheet_deg, -main.image_x_azimuth_deg);
    tilt.lean_drawing_deg = Rotate(tilt.lean_screen_deg, calibration_azimuth);
    tilt.lattice_to_plane = h;
    tilt.residual_max_px = max_distance_px;
    out.refusal = GridSheetRefusal::none;
    out.tilt = tilt;
    return out;
}

GridSheetPairOutcome CompareGridSheetTilts(const GridSheetTiltOutcome& reference, const GridSheetTiltOutcome& target, const GridSheetPairAcceptance& acceptance) {
    Require(FiniteNonNegative(acceptance.max_rotation_mismatch_deg) && acceptance.max_rotation_mismatch_deg < 45,
        "grid sheet tilt: the rotation mismatch line is not set");
    GridSheetPairOutcome out;
    if (!reference.tilt) { out.refusal = GridSheetPairRefusal::reference_not_measured; return out; }
    if (!target.tilt) { out.refusal = GridSheetPairRefusal::target_not_measured; return out; }
    const GridSheetTilt& a = *reference.tilt;
    const GridSheetTilt& b = *target.tilt;
    const double calibrated_difference = Wrap180(b.calibration_x_azimuth_deg - a.calibration_x_azimuth_deg);
    std::array<double, 4> mismatch{};
    int best = 0;
    for (int k = 0; k < 4; ++k) {
        mismatch[static_cast<std::size_t>(k)] = Wrap180(b.image_x_azimuth_deg + 90.0 * k - a.image_x_azimuth_deg - calibrated_difference);
        if (std::abs(mismatch[static_cast<std::size_t>(k)]) < std::abs(mismatch[static_cast<std::size_t>(best)])) best = k;
    }
    out.rotation_mismatch_deg = mismatch[static_cast<std::size_t>(best)];
    // A mismatch below 45 degrees also makes the quarter turn unique.
    if (!(std::abs(out.rotation_mismatch_deg) <= acceptance.max_rotation_mismatch_deg)) { out.refusal = GridSheetPairRefusal::turn_mismatch; return out; }

    // Both leans in the reference's sheet frame, then the optical axes as unit vectors (toward the sheet).
    const Point lean_a = a.lean_sheet_deg;
    const Point lean_b = Rotate(b.lean_sheet_deg, 90.0 * best);
    const auto axis = [](Point lean, double tilt_deg) {
        const double n = std::hypot(lean.x, lean.y);
        const double s = std::sin(tilt_deg * detail::kDegrees), c = std::cos(tilt_deg * detail::kDegrees);
        const Point dir = n > 0 ? Point{lean.x / n, lean.y / n} : Point{0, 0};
        return std::array<double, 3>{s * dir.x, s * dir.y, c};
    };
    const auto ua = axis(lean_a, a.tilt_deg), ub = axis(lean_b, b.tilt_deg);
    const double dot = ua[0] * ub[0] + ua[1] * ub[1] + ua[2] * ub[2];
    const double cx = ua[1] * ub[2] - ua[2] * ub[1], cy = ua[2] * ub[0] - ua[0] * ub[2], cz = ua[0] * ub[1] - ua[1] * ub[0];
    GridSheetPair pair;
    pair.reference_camera = a.camera; pair.target_camera = b.camera;
    pair.optical_axis_angle_deg = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), dot) / detail::kDegrees;
    const double to_drawing = Wrap180(a.calibration_x_azimuth_deg - a.image_x_azimuth_deg);
    const Point diff{lean_b.x - lean_a.x, lean_b.y - lean_a.y};
    pair.lean_difference_drawing_deg = Rotate(diff, to_drawing);
    pair.relative_rotation_deg = Wrap180(b.image_x_azimuth_deg + 90.0 * best - a.image_x_azimuth_deg);
    pair.quarter_turns = best;
    pair.rotation_mismatch_deg = out.rotation_mismatch_deg;
    out.refusal = GridSheetPairRefusal::none;
    out.pair = pair;
    return out;
}

}   // namespace a0::m2::experimental::grid_sheet_tilt
