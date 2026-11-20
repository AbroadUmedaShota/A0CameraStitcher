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

void ValidateProjection(const render::CameraProjection& p) {
    const auto& i = p.intrinsics;
    Require(std::isfinite(i.fx_pixels) && std::isfinite(i.fy_pixels) && i.fx_pixels > 0 && i.fy_pixels > 0 &&
        std::isfinite(i.cx_pixels) && std::isfinite(i.cy_pixels), "grid sheet tilt: the projection has no valid intrinsics");
}
void ValidateSettings(const GridSheetTiltSettings& s) {
    const auto& d = s.detection;
    Require(std::isfinite(d.pitch_min_px) && std::isfinite(d.pitch_max_px) && d.pitch_min_px > 4 && d.pitch_max_px > d.pitch_min_px,
        "grid sheet tilt: the pitch range is not set");
    Require(d.window_half_size > 0 && d.window_min_points >= 1 && d.direction_min_points >= 1 && d.fit_max_points >= 100 && d.fit_iterations >= 1 &&
        d.line_min_points >= 3 && d.node_min_count >= 8 && d.coverage_cell_px >= 1 && d.flat_cells_across >= 1,
        "grid sheet tilt: the detection settings are not set");
    Require(d.pitch_coarse_divisor >= 4 && d.pitch_fine_step > 0 && d.pitch_fine_span > 0 && d.window_scan_points >= 50 && d.init_scan_points >= 100,
        "grid sheet tilt: the pitch scan settings are not set");
    Require(d.direction_gate_deg > 0 && d.direction_gate_deg < 45 && d.direction_match_deg > 0 && d.direction_match_deg < 45 && d.final_gate_px > 0,
        "grid sheet tilt: the gates are not set");
    const auto& a = s.acceptance;
    const auto finite = [](double v) { return std::isfinite(v) && v >= 0; };
    Require(finite(a.max_residual_rms_px) && finite(a.max_method_spread_deg) && finite(a.min_visible_fraction) && finite(a.max_off_lattice_cell_fraction) &&
        finite(a.off_lattice_offset_px) && a.min_lines_per_family >= 2, "grid sheet tilt: the acceptance is not set");
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
    // accepted optical threshold or a statement about what tilt is acceptable.
    s.acceptance.min_ridge_points = 20000;
    s.acceptance.min_lines_per_family = 20;
    s.acceptance.min_lattice_span = 20;
    s.acceptance.min_visible_fraction = 0.05;
    s.acceptance.max_residual_rms_px = 1.5;
    s.acceptance.max_method_spread_deg = 0.3;
    s.acceptance.max_off_lattice_cell_fraction = 0.15;
    s.acceptance.off_lattice_offset_px = 4.0;
    return s;
}

std::string_view RefusalCode(GridSheetRefusal refusal) {
    switch (refusal) {
    case GridSheetRefusal::none: return "none";
    case GridSheetRefusal::too_few_ridge_points: return "too-few-ridge-points";
    case GridSheetRefusal::no_grid_found: return "no-grid-found";
    case GridSheetRefusal::fit_failed: return "fit-failed";
    case GridSheetRefusal::too_few_lines: return "too-few-lines";
    case GridSheetRefusal::span_too_small: return "span-too-small";
    case GridSheetRefusal::view_too_small: return "view-too-small";
    case GridSheetRefusal::residual_too_large: return "residual-too-large";
    case GridSheetRefusal::methods_disagree: return "methods-disagree";
    case GridSheetRefusal::sheet_not_flat: return "sheet-not-flat";
    }
    throw std::invalid_argument("unknown grid sheet refusal");
}

std::string_view RefusalCode(GridSheetPairRefusal refusal) {
    switch (refusal) {
    case GridSheetPairRefusal::none: return "none";
    case GridSheetPairRefusal::reference_not_measured: return "reference-not-measured";
    case GridSheetPairRefusal::target_not_measured: return "target-not-measured";
    case GridSheetPairRefusal::quarter_turn_ambiguous: return "quarter-turn-ambiguous";
    }
    throw std::invalid_argument("unknown grid sheet pair refusal");
}

GridSheetTiltOutcome MeasureGridSheetTilt(std::string_view camera, const ImageView& picture,
    const render::CameraProjection& profile_projection, const GridSheetTiltSettings& settings) {
    ValidateProjection(profile_projection);
    ValidateSettings(settings);
    const auto& det = settings.detection;
    const auto& acc = settings.acceptance;
    GridSheetTiltOutcome out;
    out.refusal = GridSheetRefusal::fit_failed;
    auto& ev = out.evidence;
    const auto refuse = [&](GridSheetRefusal why) { out.refusal = why; out.tilt.reset(); return out; };

    // The calibrated turn of the image axes: a programming error when the profile itself is not a valid projection.
    const double calibration_azimuth = content_registration::OrientationOf(profile_projection).rotation_clockwise_deg;

    // (a) lines
    const detail::RidgeResult ridge = detail::DetectRidges(picture, det);
    ev.ridge_points = ridge.points.size();
    if (ridge.points.size() < std::max<std::size_t>(acc.min_ridge_points, 1)) return refuse(GridSheetRefusal::too_few_ridge_points);

    // (c) points without the lens distortion
    const detail::Lens lens = detail::LensOf(profile_projection);
    const double focal = lens.MeanFocal();
    const detail::PreparedPoints points = detail::PreparePoints(ridge.points, lens, true);

    // (b) first lattice
    const auto initial = detail::FindInitialLattice(points, focal, det);
    if (!initial) return refuse(GridSheetRefusal::no_grid_found);
    ev.pitch_px = initial->pitch_px;

    // (d) the lattice
    const detail::LatticeFit fit = detail::FitLattice(points, focal, *initial, det);
    if (!fit.ok) return refuse(GridSheetRefusal::fit_failed);
    const detail::Mat3 h = detail::LatticeToPlane(fit.parameters);
    if (!detail::PositivelyOriented(h)) return refuse(GridSheetRefusal::fit_failed);

    const detail::Assignment assignment = detail::AssignPoints(fit.parameters, points, focal, det.direction_match_deg, det.final_gate_px);
    std::array<std::int32_t, 2> lowest{INT32_MAX, INT32_MAX}, highest{INT32_MIN, INT32_MIN};
    double sum_sq = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!assignment.ok[i]) continue;
        ++ev.used_points;
        sum_sq += static_cast<double>(assignment.distance_px[i]) * assignment.distance_px[i];
        const auto f = assignment.family[i];
        lowest[f] = std::min(lowest[f], assignment.index[i]); highest[f] = std::max(highest[f], assignment.index[i]);
    }
    if (ev.used_points == 0) return refuse(GridSheetRefusal::fit_failed);
    ev.residual_rms_px = std::sqrt(sum_sq / static_cast<double>(ev.used_points));
    for (int f = 0; f < 2; ++f) ev.lattice_span[f] = highest[f] - lowest[f] + 1;
    ev.visible_fraction = detail::VisibleFraction(ridge.points, points, assignment, picture.width, picture.height, det.coverage_cell_px, det.coverage_cell_min_points);

    // (e) the tilt, three ways
    const detail::OrientationNumbers main = detail::OrientationOfLattice(profile_projection, h);
    if (!main.ok) return refuse(GridSheetRefusal::fit_failed);
    ev.tilt_deg_fit = main.tilt_deg;
    const auto lines = detail::FitLines(points, assignment, focal, det.line_min_points);
    for (const auto& l : lines) ++ev.lines[static_cast<std::size_t>(l.family)];
    const detail::NodeFit nodes = detail::FitNodes(lines, h, focal, det);
    ev.nodes = nodes.nodes;
    detail::OrientationNumbers by_nodes;
    if (nodes.ok) {
        ev.node_residual_rms_px = nodes.rms_px;
        by_nodes = detail::OrientationOfLattice(profile_projection, nodes.lattice_to_plane);
        if (by_nodes.ok) ev.tilt_deg_nodes = by_nodes.tilt_deg;
    }
    const detail::VanishingFit vanishing = detail::FitVanishing(lines, h);
    if (vanishing.ok) ev.tilt_deg_vanishing = vanishing.tilt_deg;
    if (nodes.ok && by_nodes.ok && vanishing.ok) {
        detail::OrientationNumbers v;
        v.lean_sheet_deg = vanishing.lean_sheet_deg;
        ev.method_spread_deg = std::max({LeanDistance(main, by_nodes), LeanDistance(main, v), LeanDistance(by_nodes, v)});
    }
    const detail::FlatnessCheck flat = detail::CheckFlatness(fit.parameters, points, ridge.points, picture.width, picture.height, focal, acc.off_lattice_offset_px, det);
    ev.off_lattice_cell_fraction = flat.off_lattice_fraction;
    ev.judged_cells = flat.judged_cells;

    // The caller's lines decide whether the value is shown.
    if (ev.lines[0] < acc.min_lines_per_family || ev.lines[1] < acc.min_lines_per_family || !nodes.ok || !by_nodes.ok || !vanishing.ok)
        return refuse(GridSheetRefusal::too_few_lines);
    if (ev.lattice_span[0] < static_cast<std::int64_t>(acc.min_lattice_span) || ev.lattice_span[1] < static_cast<std::int64_t>(acc.min_lattice_span))
        return refuse(GridSheetRefusal::span_too_small);
    if (ev.visible_fraction < acc.min_visible_fraction) return refuse(GridSheetRefusal::view_too_small);
    if (ev.residual_rms_px > acc.max_residual_rms_px) return refuse(GridSheetRefusal::residual_too_large);
    if (ev.off_lattice_cell_fraction > acc.max_off_lattice_cell_fraction) return refuse(GridSheetRefusal::sheet_not_flat);
    if (ev.method_spread_deg > acc.max_method_spread_deg) return refuse(GridSheetRefusal::methods_disagree);

    GridSheetTilt tilt;
    tilt.camera = std::string(camera);
    tilt.tilt_deg = main.tilt_deg;
    tilt.lean_sheet_deg = main.lean_sheet_deg;
    tilt.image_x_azimuth_deg = main.image_x_azimuth_deg;
    tilt.calibration_x_azimuth_deg = calibration_azimuth;
    tilt.lean_screen_deg = Rotate(main.lean_sheet_deg, -main.image_x_azimuth_deg);
    tilt.lean_drawing_deg = Rotate(tilt.lean_screen_deg, calibration_azimuth);
    out.refusal = GridSheetRefusal::none;
    out.tilt = tilt;
    return out;
}

GridSheetPairOutcome CompareGridSheetTilts(const GridSheetTiltOutcome& reference, const GridSheetTiltOutcome& target, const GridSheetPairAcceptance& acceptance) {
    Require(std::isfinite(acceptance.min_quarter_turn_margin_deg) && acceptance.min_quarter_turn_margin_deg >= 0, "grid sheet tilt: the quarter turn margin is not set");
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
    double second = 1e300;
    for (int k = 0; k < 4; ++k) if (k != best) second = std::min(second, std::abs(mismatch[static_cast<std::size_t>(k)]));
    const double margin = second - std::abs(mismatch[static_cast<std::size_t>(best)]);
    if (margin < acceptance.min_quarter_turn_margin_deg) { out.refusal = GridSheetPairRefusal::quarter_turn_ambiguous; return out; }

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
    pair.rotation_mismatch_deg = mismatch[static_cast<std::size_t>(best)];
    pair.quarter_turn_margin_deg = margin;
    out.refusal = GridSheetPairRefusal::none;
    out.pair = pair;
    return out;
}

}   // namespace a0::m2::experimental::grid_sheet_tilt
