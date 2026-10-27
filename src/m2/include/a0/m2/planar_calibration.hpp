#pragma once

#include "a0/m2/calibration_lens.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::calibration {

// Planar calibration from flat chart placements (Issue #374, #367 stage 2).
// The chart lies flat on the document plane for every view; only its position
// and rotation on the plane change. Per camera c and view j:
//   document = T_j(chart)        T_j = rotation theta_j, translation (tx_j, ty_j)
//   undistorted pixel = H_c(document)    H_c: 3 x 3, h22 = 1 (8 unknowns)
//   pixel = K_nom * D(K_nom^-1 * undistorted pixel)   D: Brown-Conrady, 5 unknowns
// K_nom is nominal (focal length mm and sensor size), never estimated. The
// document frame is the chart frame of the overlap placement (T = I), seen by
// both cameras, so the two cameras share it.

// Declared lens and sensor values. Labels, not measurements.
struct NominalCamera {
    double focal_length_mm;  // finite, (0, 10000]
    double sensor_width_mm;  // stored-order width, finite, (0, 10000]
    double sensor_height_mm; // stored-order height, finite, (0, 10000]
};

struct PlanarCalibrationRequest {
    std::array<std::string, 2> overlap_view_ids; // CAM-A, then CAM-B: the same flat placement seen by both
    std::array<NominalCamera, 2> nominal;        // CAM-A, then CAM-B
};

// Numerical stopping criteria only, not optical or quality thresholds. No defaults.
struct PlanarCalibrationPolicy {
    std::uint32_t minimum_training_views;      // 1..128 per camera; the overlap view is not counted
    std::uint32_t max_iterations;              // 1..1000
    double relative_cost_tolerance;            // finite, strictly (0,1)
    double relative_step_tolerance;            // finite, strictly (0,1)
    double initial_damping;                    // finite, > 0
    std::uint32_t max_undistortion_iterations; // 1..1000, straightness check only
    double undistortion_tolerance_pixels;      // finite, > 0, straightness check only
};

// fx = f * width_px / sensor_width, fy = f * height_px / sensor_height,
// principal point at the image centre ((width - 1) / 2, (height - 1) / 2) of
// the stored-order sample grid, skew 0. Null for an invalid declaration.
[[nodiscard]] std::optional<PinholeIntrinsics> NominalIntrinsics(
    const NominalCamera& camera, std::uint32_t width_pixels, std::uint32_t height_pixels);

enum class PlanarCameraStatus {
    Estimated, // analytic estimate; NOT calibration approval or a quality verdict
    NominalInvalid,
    OverlapViewMissing,
    OverlapRoleMismatch,   // the overlap view must have role reference
    OverlapCameraMismatch,
    InsufficientViews,
    InitializationUnavailable,
    SolverNonconvergence,
    NumericalFailure,
    ChartNotInFront,
    InvalidDistortion
};

enum class PlanarViewOutcome {
    Evaluated,
    NotUsed, // a reference view other than the named overlap view
    InitializationUnavailable,
    SolverNonconvergence,
    NumericalFailure,
    ChartNotInFront,
    NotReached
};

constexpr std::string_view PlanarCameraStatusCode(PlanarCameraStatus status) {
    switch (status) {
    case PlanarCameraStatus::Estimated: return "planar-calibration-estimated";
    case PlanarCameraStatus::NominalInvalid: return "nominal-camera-invalid";
    case PlanarCameraStatus::OverlapViewMissing: return "overlap-view-missing";
    case PlanarCameraStatus::OverlapRoleMismatch: return "overlap-role-mismatch";
    case PlanarCameraStatus::OverlapCameraMismatch: return "overlap-camera-mismatch";
    case PlanarCameraStatus::InsufficientViews: return "insufficient-views";
    case PlanarCameraStatus::InitializationUnavailable: return "initialization-unavailable";
    case PlanarCameraStatus::SolverNonconvergence: return "solver-nonconvergence";
    case PlanarCameraStatus::NumericalFailure: return "numerical-failure";
    case PlanarCameraStatus::ChartNotInFront: return "chart-not-in-front";
    case PlanarCameraStatus::InvalidDistortion: return "invalid-distortion";
    }
    return {};
}
constexpr std::string_view PlanarViewOutcomeCode(PlanarViewOutcome outcome) {
    switch (outcome) {
    case PlanarViewOutcome::Evaluated: return "evaluated";
    case PlanarViewOutcome::NotUsed: return "not-used";
    case PlanarViewOutcome::InitializationUnavailable: return "initialization-unavailable";
    case PlanarViewOutcome::SolverNonconvergence: return "solver-nonconvergence";
    case PlanarViewOutcome::NumericalFailure: return "numerical-failure";
    case PlanarViewOutcome::ChartNotInFront: return "chart-not-in-front";
    case PlanarViewOutcome::NotReached: return "not-reached";
    }
    return {};
}

// document = R(angle) * chart + (tx, ty), in the overlap chart frame.
struct PlanarPlacement {
    double angle_radians;
    double translation_x_mm;
    double translation_y_mm;
};

// Straightness of the chart rows and columns (chart points that share x_mm or
// y_mm, three or more per line) in pixels: distance of each point from the
// total least squares line of its line. raw = detected pixels; undistorted =
// the same pixels after removing the estimated distortion with K_nom.
struct StraightnessSummary {
    double rms_pixels;
    double max_pixels;
    std::uint32_t line_count;
    std::uint32_t point_count;
};
struct Straightness {
    std::optional<StraightnessSummary> raw;
    std::optional<StraightnessSummary> undistorted; // null when a pixel cannot be undistorted
};

struct PlanarViewResult {
    std::string view_id;
    ViewRole role;
    bool overlap;                            // the named overlap view of this camera
    PlanarViewOutcome outcome;
    std::optional<PlanarPlacement> placement; // identity for the overlap view
    std::optional<ResidualSummary> residual;  // training/overlap: usedPointIds; validation: measurementPointIds
    std::optional<SolverRecord> solver;       // validation only (placement-only Levenberg-Marquardt)
    Straightness straightness;                // training/overlap: usedPointIds; validation: pose and measurement points
};

struct CameraPlanarResult {
    CameraAlias camera;
    PlanarCameraStatus status;
    std::optional<PinholeIntrinsics> nominal_intrinsics;
    std::optional<SolverRecord> solver;
    std::optional<BrownConradyDistortion> distortion;
    // Overlap chart mm -> undistorted stored-order pixel, row major, h22 = 1.
    std::optional<std::array<double, 9>> document_to_image;
    std::optional<DistortionCheck> distortion_check;
    std::vector<PlanarViewResult> views;              // every view of this camera, session order
    std::optional<ResidualSummary> fit_residual;      // pooled over the training and overlap views
    std::optional<ResidualSummary> overlap_residual;  // the overlap view alone
    HeldOutCoverage held_out_coverage;
    std::optional<ResidualSummary> held_out_residual; // pooled over evaluated validation views
    Straightness fit_straightness;
    Straightness held_out_straightness;
};

struct PlanarCalibrationResult {
    std::array<CameraPlanarResult, 2> cameras; // [0] CAM-A, [1] CAM-B
};

// The two cameras share only the overlap placement, which is fixed to T = I,
// so their problems separate exactly and each is solved by one
// Levenberg-Marquardt over H_c, the five coefficients and (theta, tx, ty) of
// every training view. Start: DLT of the overlap view for H_c, DLT of each
// training view composed with H_c^-1 for its placement, zero distortion.
// Validation views are held out: H_c and the coefficients are fixed, the
// placement is fitted to posePointIds and measurementPointIds are measured.
// Physicality (MinimumRadialSlope, DistortionJacobianDeterminant over the
// fitted points) is judged once on the converged parameters.
// Pure function: no file, clock, thread or randomness. Throws
// std::invalid_argument only for an invalid policy; otherwise only std::bad_alloc.
[[nodiscard]] PlanarCalibrationResult EstimatePlanarCalibration(const CalibrationObservationSession& session,
    const PlanarCalibrationRequest& request, const PlanarCalibrationPolicy& policy);

} // namespace a0::m2::calibration
