#pragma once

#include "a0/m2/calibration_intrinsics.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::calibration {

// Brown-Conrady, applied to undistorted normalized coordinates:
//   r2 = x^2 + y^2
//   xd = x (1 + k1 r2 + k2 r2^2 + k3 r2^3) + 2 p1 x y + p2 (r2 + 2 x^2)
//   yd = y (1 + k1 r2 + k2 r2^2 + k3 r2^3) + p1 (r2 + 2 y^2) + 2 p2 x y
// Named fields only; the OpenCV array order is deliberately not used.
enum class DistortionModel { BrownConradyK1K2K3P1P2 }; // code "brown-conrady-k1k2k3-p1p2"

struct BrownConradyDistortion {
    double k1;
    double k2;
    double k3;
    double p1;
    double p2;
};

// All fields are required; there are no defaults and the session never carries them.
struct LensIntrinsicsPolicy {
    std::uint32_t minimum_views;        // 2..128, forwarded to stage 2
    double relative_singular_cutoff;    // finite, strictly (0,1), forwarded to stage 2
    ObservationModel observation_model; // DistortedAnalytic only
    std::uint32_t max_iterations;       // 1..1000; every trial step counts, accepted or not
    double relative_cost_tolerance;     // finite, strictly (0,1)
    double relative_step_tolerance;     // finite, strictly (0,1)
    double initial_damping;             // finite, > 0
};

enum class LensStage { Initialization, Solver, Intrinsics, Distortion, Pose };

enum class LensStatus {
    LensIntrinsicsEstimated, // analytic observation; NOT a calibration success
    InitializationUnavailable,
    SolverNonconvergence,
    NumericalFailure,
    NonphysicalIntrinsics,
    InvalidDistortion,
    // Defensive. The solver only accepts parameters whose points are all in
    // front of the camera, so stage 3a has no path that reaches this status.
    PoseUnavailable
};

enum class SolverStopReason { RelativeCostReduction, RelativeStepSize, MaximumIterations, NumericalFailure };

enum class LensViewOutcome {
    Evaluated,
    InitializationUnavailable,
    SolverNonconvergence,
    NumericalFailure,
    ChartNotInFront,
    NotReached
};

// Out-of-range enum values return an empty view instead of a fabricated code.
constexpr std::string_view DistortionModelCode(DistortionModel model) {
    switch (model) {
    case DistortionModel::BrownConradyK1K2K3P1P2: return "brown-conrady-k1k2k3-p1p2";
    }
    return {};
}
constexpr std::string_view LensStageCode(LensStage stage) {
    switch (stage) {
    case LensStage::Initialization: return "initialization";
    case LensStage::Solver: return "solver";
    case LensStage::Intrinsics: return "intrinsics";
    case LensStage::Distortion: return "distortion";
    case LensStage::Pose: return "pose";
    }
    return {};
}
constexpr std::string_view LensStatusCode(LensStatus status) {
    switch (status) {
    case LensStatus::LensIntrinsicsEstimated: return "lens-intrinsics-estimated";
    case LensStatus::InitializationUnavailable: return "initialization-unavailable";
    case LensStatus::SolverNonconvergence: return "solver-nonconvergence";
    case LensStatus::NumericalFailure: return "numerical-failure";
    case LensStatus::NonphysicalIntrinsics: return "nonphysical-intrinsics";
    case LensStatus::InvalidDistortion: return "invalid-distortion";
    case LensStatus::PoseUnavailable: return "pose-unavailable";
    }
    return {};
}
// The stage that stopped a camera; null for the success status and for an
// out-of-range value. Status and stage are many to one, so only the status is stored.
constexpr std::optional<LensStage> FailedStage(LensStatus status) {
    switch (status) {
    case LensStatus::LensIntrinsicsEstimated: return std::nullopt;
    case LensStatus::InitializationUnavailable: return LensStage::Initialization;
    case LensStatus::SolverNonconvergence:
    case LensStatus::NumericalFailure: return LensStage::Solver;
    case LensStatus::NonphysicalIntrinsics: return LensStage::Intrinsics;
    case LensStatus::InvalidDistortion: return LensStage::Distortion;
    case LensStatus::PoseUnavailable: return LensStage::Pose;
    }
    return std::nullopt;
}
constexpr std::string_view SolverStopReasonCode(SolverStopReason reason) {
    switch (reason) {
    case SolverStopReason::RelativeCostReduction: return "relative-cost-reduction";
    case SolverStopReason::RelativeStepSize: return "relative-step-size";
    case SolverStopReason::MaximumIterations: return "maximum-iterations";
    case SolverStopReason::NumericalFailure: return "numerical-failure";
    }
    return {};
}
constexpr std::string_view LensViewOutcomeCode(LensViewOutcome outcome) {
    switch (outcome) {
    case LensViewOutcome::Evaluated: return "evaluated";
    case LensViewOutcome::InitializationUnavailable: return "initialization-unavailable";
    case LensViewOutcome::SolverNonconvergence: return "solver-nonconvergence";
    case LensViewOutcome::NumericalFailure: return "numerical-failure";
    case LensViewOutcome::ChartNotInFront: return "chart-not-in-front";
    case LensViewOutcome::NotReached: return "not-reached";
    }
    return {};
}

// Levenberg-Marquardt record. The costs are sums of squared raw-pixel residual
// components (px^2) and are always finite.
struct SolverRecord {
    std::uint32_t iterations;     // trial steps, accepted or rejected, Cholesky failures included
    std::uint32_t accepted_steps;
    double initial_cost;          // at the stage 2 start
    double final_cost;            // at the last accepted parameters; initial_cost if none was accepted
    SolverStopReason stop_reason;
};

// Forward-only physicality observations at the final parameters, over the used
// points of the training views.
struct DistortionCheck {
    double max_radius_squared;            // s_max
    double minimum_radial_slope;          // min over [0, s_max] of 1 + 3 k1 s + 5 k2 s^2 + 7 k3 s^3
    double minimum_jacobian_determinant;  // min over the used points of the distortion map
};

struct LensViewResult {
    std::string view_id;
    ViewRole role;                            // Training or Validation; Reference views are not listed
    LensViewOutcome outcome;
    std::optional<ChartPose> pose;            // refined (training) or held-out (validation)
    std::optional<ResidualSummary> residual;  // training: used points; validation: measurement points
    std::optional<SolverRecord> solver;       // validation only (pose-only Levenberg-Marquardt)
};

struct CameraLensResult {
    CameraAlias camera;
    LensStatus status;
    CameraIntrinsicsResult initialization;    // stage 2 of this camera, as is; always present
    std::optional<SolverRecord> solver;       // null only when the stage 2 start was not usable
    std::optional<PinholeIntrinsics> intrinsics;
    std::optional<BrownConradyDistortion> distortion;
    std::optional<DistortionCheck> distortion_check;
    std::vector<LensViewResult> views;        // training and validation views of this camera, session order
    std::optional<ResidualSummary> training_residual; // pooled over all training views
    HeldOutCoverage held_out_coverage;
    std::optional<ResidualSummary> held_out_residual; // pooled over evaluated validation views
};

// Analytic, distorted observation only. A result of status
// LensIntrinsicsEstimated is not a calibration success: the pixels were
// computed by a model, no threshold is applied to any residual, no robust
// loss or outlier rejection is used, and no profile is made.
struct LensIntrinsicsResult {
    ObservationModel observation_model;            // echo of the policy
    DistortionModel distortion_model;              // BrownConradyK1K2K3P1P2
    std::array<CameraLensResult, 2> cameras;       // [0] CAM-A, [1] CAM-B, evaluated independently
};

// Stage 2 closed form (zero distortion) is the start; Levenberg-Marquardt then
// minimises the raw-pixel reprojection error of the training views over K,
// the five coefficients and every training pose. The validation views are
// held out: K and the coefficients are fixed and only the pose is estimated
// from posePointIds before measurementPointIds are measured.
// Pure function: no file, clock, thread or randomness. Throws
// std::invalid_argument only for an invalid policy (the message has no values);
// otherwise only std::bad_alloc.
[[nodiscard]] LensIntrinsicsResult EstimateLensIntrinsics(
    const CalibrationObservationSession& session, const LensIntrinsicsPolicy& policy);

} // namespace a0::m2::calibration
