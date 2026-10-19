#pragma once

#include "a0/m2/document_render.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace a0::m2::correction {

inline constexpr std::size_t kMaximumDocumentCorrectionPoints = 4096;

// Already matched landmarks, in mm. Direction: output document -> nominal
// document coordinates of the camera being corrected. No raw pixel matching.
struct DocumentCorrespondence {
    render::Point document_mm, camera_document_mm;
    DocumentCorrespondence(render::Point document, render::Point camera) : document_mm(document), camera_document_mm(camera) {}
};

// Explicit development calculation policy, NOT a rig-profile approval gate.
// The displacement budget bounds the whole ceil raster, not just fit points.
// It does not reinterpret the existing profile's registrationError field.
struct DocumentCorrectionPolicy {
    std::size_t minimum_points;
    double minimum_rms_spread_mm, minimum_axis_variance_ratio;
    double maximum_fit_residual_output_pixels, maximum_displacement_output_pixels;
    double maximum_rotation_degrees, maximum_scale_difference_percent;
    DocumentCorrectionPolicy(std::size_t count, double spread, double axis_ratio,
        double fit, double displacement, double rotation, double scale)
        : minimum_points(count), minimum_rms_spread_mm(spread), minimum_axis_variance_ratio(axis_ratio),
          maximum_fit_residual_output_pixels(fit), maximum_displacement_output_pixels(displacement),
          maximum_rotation_degrees(rotation), maximum_scale_difference_percent(scale) {}
};

// T = [a -b tx; b a ty; 0 0 1]. Only four degrees of freedom.
struct DocumentSimilarity {
    double a, b, translation_x_mm, translation_y_mm;
};
struct DocumentCorrectionMetrics {
    std::size_t point_count;
    double rms_fit_residual_output_pixels, maximum_fit_residual_output_pixels;
    double maximum_displacement_output_pixels, rotation_degrees, scale_difference_percent;
};
enum class DocumentCorrectionOutcome {
    Estimated, InsufficientPoints, InvalidObservations, DegenerateGeometry,
    ReflectedCorrespondences, NumericalFailure, FitResidualExceeded,
    DisplacementExceeded, RotationExceeded, ScaleExceeded, ProjectionUnavailable
};
constexpr std::string_view DocumentCorrectionOutcomeCode(DocumentCorrectionOutcome value) {
    switch (value) {
    case DocumentCorrectionOutcome::Estimated: return "correction-estimated";
    case DocumentCorrectionOutcome::InsufficientPoints: return "insufficient-points";
    case DocumentCorrectionOutcome::InvalidObservations: return "invalid-observations";
    case DocumentCorrectionOutcome::DegenerateGeometry: return "degenerate-geometry";
    case DocumentCorrectionOutcome::ReflectedCorrespondences: return "reflected-correspondences";
    case DocumentCorrectionOutcome::NumericalFailure: return "numerical-failure";
    case DocumentCorrectionOutcome::FitResidualExceeded: return "fit-residual-exceeded";
    case DocumentCorrectionOutcome::DisplacementExceeded: return "displacement-exceeded";
    case DocumentCorrectionOutcome::RotationExceeded: return "rotation-exceeded";
    case DocumentCorrectionOutcome::ScaleExceeded: return "scale-exceeded";
    case DocumentCorrectionOutcome::ProjectionUnavailable: return "projection-unavailable";
    }
    return {};
}
struct DocumentCorrectionResult {
    DocumentCorrectionOutcome outcome;
    std::optional<DocumentSimilarity> estimate; // diagnostics, even if over limit
    std::optional<DocumentCorrectionMetrics> metrics;
    std::vector<DocumentCorrectionOutcome> violations;
    // Present ONLY when all explicit bounds and projection checks pass.
    std::optional<render::CameraProjection> corrected_camera;
};

// Pure geometry. All correspondences are used, no outlier pruning or clamping.
// Composes fixed H*T, preserving K/distortion. Invalid policy/base/raster throws;
// observation/fit/budget failure returns diagnostics and no usable projection.
// Does not inspect/approve a profile, assess coverage/color, or authorize capture.
[[nodiscard]] DocumentCorrectionResult EstimateBoundedDocumentCorrection(
    const render::CameraProjection& fixed_camera, const render::OutputRaster& raster,
    std::span<const DocumentCorrespondence> correspondences, const DocumentCorrectionPolicy& policy);

} // namespace a0::m2::correction
