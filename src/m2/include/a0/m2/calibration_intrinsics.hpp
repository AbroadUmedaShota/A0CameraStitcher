#pragma once

#include "a0/m2/calibration_observation_session.hpp"
#include "a0/m2/calibration_view_geometry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::calibration {

// How the observed pixels are read. Stage 2 treats every detected pixel as an
// undistorted stored-order pixel and accepts only UndistortedAnalytic.
// DistortedAnalytic belongs to the lens stage (calibration_lens.hpp) and is
// rejected here. The value 1 is never assigned: the stage 2 contract test uses
// it as an out-of-range probe.
enum class ObservationModel { UndistortedAnalytic = 0, DistortedAnalytic = 2 };

// All fields are required; there are no defaults and the session never carries them.
struct UndistortedIntrinsicsPolicy {
    std::uint32_t minimum_views;        // 2..128, forwarded to AssessViewGeometry
    double relative_singular_cutoff;    // finite, strictly (0,1), forwarded to AssessViewGeometry
    ObservationModel observation_model; // UndistortedAnalytic only
};

enum class IntrinsicsStage { Homography, ViewGeometry, Intrinsics, Pose };

enum class IntrinsicsStatus {
    UndistortedIntrinsicsEstimated, // analytic, undistorted; NOT a calibration success
    HomographyUnavailable,
    InsufficientViews,
    InsufficientViewDiversity,
    InconsistentLinearConstraints,
    NumericalFailure,
    NonphysicalIntrinsics,
    PoseUnavailable
};

enum class ViewOutcome {
    Evaluated,
    InsufficientObservations,
    DegeneratePointConfiguration,
    NumericalFailure,
    ChartNotInFront,
    NotReached
};

enum class HeldOutCoverage { NotReached, NoValidationView, AllViewsEvaluated, SomeViewsNotEvaluated };

// Out-of-range enum values return an empty view instead of a fabricated code.
constexpr std::string_view ObservationModelCode(ObservationModel model) {
    switch (model) {
    case ObservationModel::UndistortedAnalytic: return "undistorted-analytic";
    case ObservationModel::DistortedAnalytic: return "distorted-analytic";
    }
    return {};
}
constexpr std::string_view IntrinsicsStageCode(IntrinsicsStage stage) {
    switch (stage) {
    case IntrinsicsStage::Homography: return "homography";
    case IntrinsicsStage::ViewGeometry: return "view-geometry";
    case IntrinsicsStage::Intrinsics: return "intrinsics";
    case IntrinsicsStage::Pose: return "pose";
    }
    return {};
}
constexpr std::string_view IntrinsicsStatusCode(IntrinsicsStatus status) {
    switch (status) {
    case IntrinsicsStatus::UndistortedIntrinsicsEstimated: return "undistorted-intrinsics-estimated";
    case IntrinsicsStatus::HomographyUnavailable: return "homography-unavailable";
    case IntrinsicsStatus::InsufficientViews: return "insufficient-views";
    case IntrinsicsStatus::InsufficientViewDiversity: return "insufficient-view-diversity";
    case IntrinsicsStatus::InconsistentLinearConstraints: return "inconsistent-linear-constraints";
    case IntrinsicsStatus::NumericalFailure: return "numerical-failure";
    case IntrinsicsStatus::NonphysicalIntrinsics: return "nonphysical-intrinsics";
    case IntrinsicsStatus::PoseUnavailable: return "pose-unavailable";
    }
    return {};
}
// The stage that stopped a camera; null for the success status and for an
// out-of-range value. Status and stage are one to one, so only the status is stored.
constexpr std::optional<IntrinsicsStage> FailedStage(IntrinsicsStatus status) {
    switch (status) {
    case IntrinsicsStatus::UndistortedIntrinsicsEstimated: return std::nullopt;
    case IntrinsicsStatus::HomographyUnavailable: return IntrinsicsStage::Homography;
    case IntrinsicsStatus::InsufficientViews:
    case IntrinsicsStatus::InsufficientViewDiversity:
    case IntrinsicsStatus::InconsistentLinearConstraints:
    case IntrinsicsStatus::NumericalFailure: return IntrinsicsStage::ViewGeometry;
    case IntrinsicsStatus::NonphysicalIntrinsics: return IntrinsicsStage::Intrinsics;
    case IntrinsicsStatus::PoseUnavailable: return IntrinsicsStage::Pose;
    }
    return std::nullopt;
}
constexpr std::string_view ViewOutcomeCode(ViewOutcome outcome) {
    switch (outcome) {
    case ViewOutcome::Evaluated: return "evaluated";
    case ViewOutcome::InsufficientObservations: return "insufficient-observations";
    case ViewOutcome::DegeneratePointConfiguration: return "degenerate-point-configuration";
    case ViewOutcome::NumericalFailure: return "numerical-failure";
    case ViewOutcome::ChartNotInFront: return "chart-not-in-front";
    case ViewOutcome::NotReached: return "not-reached";
    }
    return {};
}
constexpr std::string_view HeldOutCoverageCode(HeldOutCoverage coverage) {
    switch (coverage) {
    case HeldOutCoverage::NotReached: return "not-reached";
    case HeldOutCoverage::NoValidationView: return "no-validation-view";
    case HeldOutCoverage::AllViewsEvaluated: return "all-validation-views-evaluated";
    case HeldOutCoverage::SomeViewsNotEvaluated: return "some-validation-views-not-evaluated";
    }
    return {};
}

// Stored-order pixel; an integer is a pixel centre. Skew is zero by model.
struct PinholeIntrinsics {
    double fx;
    double fy;
    double cx;
    double cy;
};

// Camera point = rotation * [x_mm, y_mm, 0]^T + translation_mm. Row major.
// Chart plane, chart mm.
struct ChartPose {
    std::array<double, 9> rotation;
    std::array<double, 3> translation_mm;
};

// Euclidean pixel error e_i: rms = sqrt(sum(e_i^2) / N), max = max(e_i).
// N >= 1; N = 0 is represented by a null summary, never by zeros.
struct ResidualSummary {
    double rms_pixels;
    double max_pixels;
    std::uint32_t point_count;
};

struct IntrinsicsViewResult {
    std::string view_id;
    ViewRole role; // Training or Validation; Reference views are not listed
    ViewOutcome outcome;
    // Estimated H, row major, Frobenius norm 1, w > 0 at the centroid of the points used.
    std::optional<std::array<double, 9>> chart_to_image;
    std::optional<ChartPose> pose;
    // Training: used points. Validation: measurement points.
    std::optional<ResidualSummary> residual;
};

struct CameraIntrinsicsResult {
    CameraAlias camera;
    IntrinsicsStatus status;
    std::optional<ViewGeometryAssessment> view_geometry; // #283 result as is; null if the homography stage failed
    std::optional<PinholeIntrinsics> intrinsics;
    std::vector<IntrinsicsViewResult> views;           // training and validation views of this camera, session order
    std::optional<ResidualSummary> training_residual;  // pooled over all training views
    HeldOutCoverage held_out_coverage;
    std::optional<ResidualSummary> held_out_residual;  // pooled over evaluated validation views
};

// Analytic, undistorted observation only. A result of status
// UndistortedIntrinsicsEstimated is not a calibration success: no lens
// distortion is modelled, no threshold is applied to any residual, and no
// profile is made.
struct UndistortedIntrinsicsResult {
    ObservationModel observation_model;            // echo of the policy
    std::array<CameraIntrinsicsResult, 2> cameras; // [0] CAM-A, [1] CAM-B, evaluated independently
};

// Closed-form intrinsics and per-view pose from the detected pixels of the
// training views, then the residual of the validation views (held out).
// Pure function: no file, clock, thread or randomness. Throws
// std::invalid_argument only for an invalid policy (the message has no values);
// otherwise only std::bad_alloc.
[[nodiscard]] UndistortedIntrinsicsResult EstimateUndistortedIntrinsics(
    const CalibrationObservationSession& session, const UndistortedIntrinsicsPolicy& policy);

} // namespace a0::m2::calibration
