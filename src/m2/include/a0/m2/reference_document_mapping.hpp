#pragma once

#include "a0/m2/calibration_lens.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace a0::m2::calibration {

// Known document-mm -> reference-chart-mm placement. The chart must lie on
// the document plane. This declaration is an input, not a measured proof.
// chart = R(angle_radians) * document + (translation_x_mm, translation_y_mm).
struct DocumentPlanePlacement {
    double angle_radians;
    double translation_x_mm;
    double translation_y_mm;
    bool coplanar_confirmed;
};

struct ReferencePairRequest {
    std::array<std::string, 2> view_ids; // explicitly CAM-A, then CAM-B
    std::optional<DocumentPlanePlacement> placement;
};

// Numerical stopping criteria, not optical or quality acceptance thresholds.
// No defaults. The lens stage learns from training views only; reference
// refinement fixes K and all five distortion coefficients.
struct ReferenceMappingPolicy {
    LensIntrinsicsPolicy lens;
    std::uint32_t max_undistortion_iterations; // 1..1000
    double undistortion_tolerance_pixels;    // finite, > 0
};

enum class ReferenceMappingOutcome {
    NotReached, MappingEstimated, ReferenceMissing, ReferenceRoleMismatch,
    ReferenceCameraMismatch, PlacementMissing, PlacementInvalid,
    LensUnavailable, UndistortionUnavailable, PoseUnavailable,
    SolverNonconvergence, NumericalFailure
};
constexpr std::string_view ReferenceMappingOutcomeCode(ReferenceMappingOutcome outcome) {
    switch (outcome) {
    case ReferenceMappingOutcome::NotReached: return "not-reached";
    case ReferenceMappingOutcome::MappingEstimated: return "mapping-estimated";
    case ReferenceMappingOutcome::ReferenceMissing: return "reference-missing";
    case ReferenceMappingOutcome::ReferenceRoleMismatch: return "reference-role-mismatch";
    case ReferenceMappingOutcome::ReferenceCameraMismatch: return "reference-camera-mismatch";
    case ReferenceMappingOutcome::PlacementMissing: return "reference-placement-missing";
    case ReferenceMappingOutcome::PlacementInvalid: return "reference-placement-invalid";
    case ReferenceMappingOutcome::LensUnavailable: return "lens-unavailable";
    case ReferenceMappingOutcome::UndistortionUnavailable: return "undistortion-unavailable";
    case ReferenceMappingOutcome::PoseUnavailable: return "pose-unavailable";
    case ReferenceMappingOutcome::SolverNonconvergence: return "solver-nonconvergence";
    case ReferenceMappingOutcome::NumericalFailure: return "numerical-failure";
    }
    return {};
}

struct ReferenceViewMappingResult {
    CameraAlias camera;
    std::string view_id;
    ReferenceMappingOutcome outcome;
    std::optional<ChartPose> pose;
    std::optional<ResidualSummary> residual; // raw pixel fit of usedPointIds
    std::optional<SolverRecord> solver;
};

struct FixedDocumentCameraMapping {
    CameraAlias camera;
    std::string reference_view_id;
    std::string reference_image_sha256;
    PinholeIntrinsics intrinsics;
    BrownConradyDistortion distortion;
    // Row major, column vectors, document mm -> undistorted stored-order
    // pixel. H(reference pose) * T(placement), normalized to element 8 == 1.
    std::array<double, 9> document_to_image;
};

struct ReferenceDocumentMappingResult {
    std::array<ReferenceViewMappingResult, 2> cameras;
    // Absent when reference/placement preflight failed before lens estimation.
    std::optional<LensIntrinsicsResult> lens;
    // Present only when BOTH reference mappings were estimated. Diagnostics
    // of a successful leg may remain when the other leg failed.
    std::optional<std::array<FixedDocumentCameraMapping, 2>> mapping;
};

// Development-only pure calculation (Issue #312, #277 stage 4). No I/O,
// clock, randomness, OpenCV, profile writer or product capture operation.
// K and distortion are estimated from THIS immutable session, never from a
// supplied/foreign calibration result or a reference view. mapping-estimated
// is not calibration approval, a draft profile, or an A0 quality verdict.
[[nodiscard]] ReferenceDocumentMappingResult EstimateReferenceDocumentMapping(
    const CalibrationObservationSession& session, const ReferencePairRequest& request,
    const ReferenceMappingPolicy& policy);

} // namespace a0::m2::calibration
